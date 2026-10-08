# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""dut.py - flexptpd as the device under test, started per check:

    flexptpd -i IF [-c CONFIG] [-s] [-O NS] [-D PPB] [-P PRIO] [-m NAME] [-q] [-v]

-s runs it on software timestamps and a virtual clock over CLOCK_REALTIME
(offset -O, drift -D); -v prints "vclock-error-ns N" once a second, the
virtual clock minus CLOCK_REALTIME, which is the ground truth of its time
error against the tester. The status block is /dev/shm/NAME.
"""

import os
import re
import signal
import subprocess
import threading
import time
from typing import Callable, Optional

from status import Status, StatusReader

VCLOCK_RE = re.compile(r"vclock-error-ns\s+(-?\d+)")

# the configuration every check starts from (the daemon loads the gPTP preset first)
BASE_CONFIG = [
    "ptp log info on",
    "ptp log bmca on",
]


class DutError(Exception):
    pass


class Dut:
    def __init__(self, binary: str, ifname: str, workdir: str, *, name: str = "dut",
                 config: Optional[list] = None, sw: bool = True, offset_ns: int = 0,
                 drift_ppb: int = 0, prio: Optional[int] = None, verbose: bool = True,
                 extra_args: Optional[list] = None):
        self.binary = binary
        self.ifname = ifname
        self.workdir = workdir
        self.name = name

        self.config = BASE_CONFIG + (config or [])
        self.sw = sw
        self.offset_ns = offset_ns
        self.drift_ppb = drift_ppb
        self.prio = prio
        self.verbose = verbose
        self.extra_args = extra_args or []

        self.shm_name = "flexptpd-conf-%d-%s" % (os.getpid(), name)
        self.log_path = os.path.join(workdir, name + ".log")
        self.cfg_path = os.path.join(workdir, name + ".cfg")

        self.proc: Optional[subprocess.Popen] = None
        self.reader = StatusReader(self.shm_name)

        self._log_pos = 0
        self._vclock = []                # (monotonic time it was printed, within 50 ms; error ns)
        self._vclock_lock = threading.Lock()
        self._follow_stop = threading.Event()
        self._follower: Optional[threading.Thread] = None

    # ---- life cycle ----

    def argv(self) -> list:
        args = [self.binary, "-i", self.ifname, "-c", self.cfg_path, "-m", self.shm_name]

        if self.sw:
            args += ["-s", "-O", str(self.offset_ns), "-D", str(self.drift_ppb)]

        if self.prio is not None:
            args += ["-P", str(self.prio)]

        if self.verbose:
            args += ["-v"]

        return args + self.extra_args

    def start(self, wait_status: float = 5.0) -> None:
        with open(self.cfg_path, "w") as f:
            f.write("# written by the conformance suite\n")
            f.write("\n".join(self.config) + "\n")

        self.log = open(self.log_path, "w")

        self.proc = subprocess.Popen(
            self.argv(),
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )

        self._follower = threading.Thread(target=self._follow, daemon=True)
        self._follower.start()

        if wait_status <= 0:
            return

        deadline = time.monotonic() + wait_status

        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise DutError("flexptpd exited with %d at start: %s" % (self.proc.returncode, self.tail()))

            if self.reader.read() is not None:
                return

            time.sleep(0.05)

        raise DutError("no status block /dev/shm/%s within %.0f s: %s" % (self.shm_name, wait_status, self.tail()))

    def stop(self, timeout: float = 1.0) -> Optional[float]:
        """SIGTERM; the seconds it took to exit, None if it had to be killed."""
        if self.proc is None:
            return 0.0

        took = None

        if self.proc.poll() is None:
            t0 = time.monotonic()
            self.proc.send_signal(signal.SIGTERM)

            try:
                self.proc.wait(timeout)
                took = time.monotonic() - t0
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

        else:
            took = 0.0

        self._follow_stop.set()

        if self._follower is not None:
            self._follower.join(timeout=1.0)

        self.reader.close()
        self.log.close()

        shm = "/dev/shm/" + self.shm_name
        if os.path.exists(shm):
            try:
                os.unlink(shm)
            except OSError:
                pass

        return took

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    @property
    def pid(self) -> int:
        return self.proc.pid

    # ---- observation ----

    def status(self) -> Optional[Status]:
        return self.reader.read()

    def wait_status(self, pred: Callable, timeout: float, poll: float = 0.05) -> Optional[Status]:
        """The first status for which pred holds, or None after timeout."""
        deadline = time.monotonic() + timeout

        while True:
            st = self.status()

            if st is not None and pred(st):
                return st

            if time.monotonic() >= deadline or not self.alive():
                return None

            time.sleep(poll)

    def identity(self, timeout: float = 3.0) -> bytes:
        """Our clockIdentity, as soon as the daemon has published it."""
        st = self.wait_status(lambda s: s.ownIdentity != 0, timeout)

        if st is None:
            raise DutError("no clockIdentity in the status block: %r" % self.status())

        return st.own_identity

    def _read_log(self) -> None:
        try:
            with open(self.log_path, "r", errors="replace") as f:
                f.seek(self._log_pos)
                text = f.read()
        except OSError:
            return

        # whole lines only; a partial one waits for the next read
        end = text.rfind("\n") + 1
        self._log_pos += len(text[:end].encode("utf-8", errors="replace"))

        now = time.monotonic()

        with self._vclock_lock:
            for m in VCLOCK_RE.finditer(text[:end]):
                self._vclock.append((now, int(m.group(1))))

    def _follow(self) -> None:
        while not self._follow_stop.wait(0.05):
            self._read_log()

    def vclock_errors(self, since: Optional[float] = None) -> list:
        """The "vclock-error-ns" values printed after `since` (monotonic)."""
        self._read_log()

        with self._vclock_lock:
            return [e for t, e in self._vclock if since is None or t >= since]

    def tail(self, n: int = 600) -> str:
        try:
            with open(self.log_path, "r", errors="replace") as f:
                return f.read()[-n:].replace("\n", " | ")
        except OSError:
            return ""

    def cpu_seconds(self) -> float:
        """utime + stime of the process, all threads."""
        with open("/proc/%d/stat" % self.pid) as f:
            fields = f.read().rsplit(")", 1)[1].split()

        ticks = os.sysconf("SC_CLK_TCK")
        return (int(fields[11]) + int(fields[12])) / ticks
