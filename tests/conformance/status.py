# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""status.py - read flexptpd's status block, /dev/shm/NAME:

    struct {
        char magic[8];          // "FLXPTPD\\0"
        uint32_t layout;        // PTP_GPTP_STATUS_VERSION
        uint32_t seq;           // seqlock: odd while the daemon writes
        uint32_t size;          // sizeof(PtpGptpStatus)
        uint32_t _r;
        PtpGptpStatus status;   // src/flexptp/gptp_status.h
    };

PtpGptpStatus is mirrored with ctypes (natural alignment, as gcc lays it out
on x86-64 and aarch64). check_layout() compares the mirror with the C header
itself when a C compiler is at hand.

Run it directly for the self-test:  python3 -I status.py
"""

import ctypes
import mmap
import os
import shutil
import subprocess
import tempfile
import time
from typing import Optional

MAGIC = b"FLXPTPD\0"
LAYOUT = 1

# PtpBmcaFsmState
PORT_STATES = {
    0: "INITIALIZING",
    1: "LISTENING",
    2: "PRE_MASTER",
    3: "MASTER",
    4: "SLAVE",
    5: "PASSIVE",
    6: "UNCALIBRATED",
    7: "FAULTY",
    8: "DISABLED",
}
MASTER = 3
SLAVE = 4
LISTENING = 1


class PtpGptpStatus(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("updates", ctypes.c_uint32),

        ("portState", ctypes.c_uint8),
        ("linkUp", ctypes.c_uint8),
        ("asCapable", ctypes.c_uint8),
        ("isMeasuringDelay", ctypes.c_uint8),
        ("domain", ctypes.c_uint8),
        ("gmPresent", ctypes.c_uint8),
        ("locked", ctypes.c_uint8),
        ("_r0", ctypes.c_uint8),

        ("ownIdentity", ctypes.c_uint64),

        ("gmIdentity", ctypes.c_uint64),
        ("gmPriority1", ctypes.c_uint8),
        ("gmClockClass", ctypes.c_uint8),
        ("gmClockAccuracy", ctypes.c_uint8),
        ("gmPriority2", ctypes.c_uint8),
        ("gmVariance", ctypes.c_uint16),
        ("stepsRemoved", ctypes.c_uint16),
        ("gmTimeBaseIndicator", ctypes.c_uint16),
        ("_r1", ctypes.c_uint16),
        ("gmChanges", ctypes.c_uint32),

        ("meanLinkDelay_ns", ctypes.c_int64),
        ("neighborRateRatio", ctypes.c_double),
        ("rateRatio", ctypes.c_double),
        ("timeError_ns", ctypes.c_int64),
        ("tuning_ppb", ctypes.c_double),
        ("logSyncInterval", ctypes.c_int8),
        ("logPdelayInterval", ctypes.c_int8),
        ("logAnnounceInterval", ctypes.c_int8),
        ("_r2", ctypes.c_uint8),

        ("syncRx", ctypes.c_uint32),
        ("syncTimeouts", ctypes.c_uint32),
        ("announceRx", ctypes.c_uint32),
        ("announceTimeouts", ctypes.c_uint32),
        ("pdelayLost", ctypes.c_uint32),
        ("pdelayMultiple", ctypes.c_uint32),
        ("signalingRx", ctypes.c_uint32),
        ("txTimestampsLost", ctypes.c_uint32),

        ("rsyncEnabled", ctypes.c_uint8),
        ("rsyncDomain", ctypes.c_uint8),
        ("_r3", ctypes.c_uint16),
        ("rsyncInterval_us", ctypes.c_uint32),
        ("rsyncSent", ctypes.c_uint32),
    ]


class ShmHeader(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_char * 8),
        ("layout", ctypes.c_uint32),
        ("seq", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("_r", ctypes.c_uint32),
    ]


STATUS_OFFSET = ctypes.sizeof(ShmHeader)


class Status:
    """One consistent snapshot, with the identities as wire-order bytes."""

    def __init__(self, raw: PtpGptpStatus):
        self.raw = raw

        for name, _ in PtpGptpStatus._fields_:
            if not name.startswith("_"):
                setattr(self, name, getattr(raw, name))

        self.own_identity = raw.ownIdentity.to_bytes(8, "big")
        self.gm_identity = raw.gmIdentity.to_bytes(8, "big")
        self.state_name = PORT_STATES.get(raw.portState, str(raw.portState))

    def __repr__(self) -> str:
        return (
            f"<{self.state_name} link={self.linkUp} asCapable={self.asCapable}"
            f" gm={self.gm_identity.hex()} mld={self.meanLinkDelay_ns}"
            f" nrr={self.neighborRateRatio:.9f} te={self.timeError_ns} upd={self.updates}>"
        )


class StatusReader:
    def __init__(self, name: str):
        self.path = "/dev/shm/" + name.lstrip("/")
        self._mm = None

    def _map(self) -> bool:
        if self._mm is not None:
            return True

        try:
            fd = os.open(self.path, os.O_RDONLY)
        except FileNotFoundError:
            return False

        try:
            size = os.fstat(fd).st_size

            if size < STATUS_OFFSET + ctypes.sizeof(PtpGptpStatus):
                return False

            self._mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)

        return True

    def read(self, retries: int = 50) -> Optional[Status]:
        """A consistent snapshot, or None when the block is missing or invalid."""
        if not self._map():
            return None

        for _ in range(retries):
            hdr = ShmHeader.from_buffer_copy(self._mm[:STATUS_OFFSET])

            if hdr.magic != MAGIC.rstrip(b"\0") or hdr.layout != LAYOUT:
                return None

            if hdr.size != ctypes.sizeof(PtpGptpStatus):
                return None

            if hdr.seq & 1:
                time.sleep(0.0002)
                continue

            raw = PtpGptpStatus.from_buffer_copy(
                self._mm[STATUS_OFFSET:STATUS_OFFSET + ctypes.sizeof(PtpGptpStatus)]
            )

            seq_after = ShmHeader.from_buffer_copy(self._mm[:STATUS_OFFSET]).seq

            if seq_after == hdr.seq:
                return Status(raw)

        return None

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
            self._mm = None


# ---- layout check against the C header -----------------------------------

C_PROBE = r"""
#include <stddef.h>
#include <stdio.h>
#include "flexptp/gptp_status.h"
#define F(f) printf("%s %zu\n", #f, offsetof(PtpGptpStatus, f));
int main(void) {
    printf("sizeof %zu\n", sizeof(PtpGptpStatus));
    printf("version_macro %d\n", PTP_GPTP_STATUS_VERSION);
    FIELDS
    return 0;
}
"""


def check_layout(src_dir: str) -> tuple:
    """(ok, detail): the ctypes mirror against src/flexptp/gptp_status.h."""
    cc = shutil.which("cc") or shutil.which("gcc")

    if cc is None:
        return True, "no C compiler, layout not checked"

    names = [n for n, _ in PtpGptpStatus._fields_]
    probe = C_PROBE.replace("FIELDS", "\n    ".join("F(%s)" % n for n in names))

    with tempfile.TemporaryDirectory() as tmp:
        c_file = os.path.join(tmp, "probe.c")
        exe = os.path.join(tmp, "probe")

        with open(c_file, "w") as f:
            f.write(probe)

        r = subprocess.run([cc, "-I", src_dir, "-o", exe, c_file], capture_output=True, text=True)

        if r.returncode != 0:
            return False, "probe did not compile: " + r.stderr.strip()[:300]

        out = subprocess.run([exe], capture_output=True, text=True).stdout

    c = dict(line.split() for line in out.splitlines())
    errors = []

    if int(c["sizeof"]) != ctypes.sizeof(PtpGptpStatus):
        errors.append("sizeof C %s, mirror %d" % (c["sizeof"], ctypes.sizeof(PtpGptpStatus)))

    if int(c["version_macro"]) != LAYOUT:
        errors.append("PTP_GPTP_STATUS_VERSION %s, mirror %d" % (c["version_macro"], LAYOUT))

    for n in names:
        mine = getattr(PtpGptpStatus, n).offset

        if int(c[n]) != mine:
            errors.append("%s at %s in C, %d in the mirror" % (n, c[n], mine))

    if errors:
        return False, "; ".join(errors)

    return True, "sizeof %d, %d fields at the same offsets" % (ctypes.sizeof(PtpGptpStatus), len(names))


# ---- self-test ------------------------------------------------------------

def write_block(path: str, st: PtpGptpStatus, seq: int = 2) -> None:
    """A status block as the daemon writes it (for tests)."""
    hdr = ShmHeader(MAGIC.rstrip(b"\0"), LAYOUT, seq, ctypes.sizeof(PtpGptpStatus), 0)

    with open(path, "wb") as f:
        f.write(bytes(hdr) + bytes(st))


def _selftest() -> int:
    failures = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures

        print(("ok    " if ok else "FAIL  ") + name + (": " + detail if detail else ""))

        if not ok:
            failures += 1

    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.normpath(os.path.join(here, "..", "..", "src"))

    ok, detail = check_layout(src)
    check("mirror matches gptp_status.h", ok, detail)

    name = "flexptpd-selftest-%d" % os.getpid()
    path = "/dev/shm/" + name

    st = PtpGptpStatus()
    st.version = LAYOUT
    st.portState = SLAVE
    st.asCapable = 1
    st.ownIdentity = int.from_bytes(bytes.fromhex("020000fffe000001"), "big")
    st.gmIdentity = int.from_bytes(bytes.fromhex("3cc0c6fffefe0210"), "big")
    st.meanLinkDelay_ns = 203
    st.neighborRateRatio = 1.000001

    try:
        write_block(path, st)
        s = StatusReader(name).read()
        check("snapshot read back", s is not None and s.portState == SLAVE and s.asCapable == 1
              and s.gm_identity == bytes.fromhex("3cc0c6fffefe0210") and s.meanLinkDelay_ns == 203,
              repr(s))

        write_block(path, st, seq=3)
        check("odd seqlock reads as busy", StatusReader(name).read(retries=3) is None)
    finally:
        if os.path.exists(path):
            os.unlink(path)

    check("missing block reads as None", StatusReader(name).read() is None)

    print("SELFTEST: " + ("PASS" if failures == 0 else "FAIL (%d)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_selftest())
