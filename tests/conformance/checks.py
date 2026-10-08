# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""checks.py - the conformance checks of tickets G1 to G5 (#20 to #24),
against flexptpd on the DUT end of a veth pair, the tester on the other end.

Every check function takes the run context and returns a list of
(check id, ok, detail). Tolerances are the constants below: software
timestamps in a network namespace jitter by tens of microseconds, so the
time checks are loose here and tight on the bench (G3.5, G5.4).
"""

import statistics
import subprocess
import threading
import time
from typing import Callable, Optional

import gptp
from dut import Dut
from status import MASTER, SLAVE, LISTENING
from tester import (Endpoint, GrandMaster, PdelayInitiator, PdelayResponder,
                    ReverseSyncReceiver, intervals, median, now_ns, send_signaling, slope)

# ---- tolerances and timings -------------------------------------------------

# veth with software timestamps measures microseconds of link delay, above
# 802.1AS's 800 ns: the suite raises neighborPropDelayThresh to this
VETH_NBTHRESH_NS = 100_000

INTERVAL_TOL = 0.10              # message intervals: +/-10 %
RSYNC_INTERVAL_TOL = 0.05        # reverse sync intervals: +/-5 %
NRR_TOL_PPM = 1.0                # neighborRateRatio against the known rates
MLD_MAX_NS = 100_000             # a sane link delay on veth, software timestamps
MLD_AGREE_NS = 50_000            # DUT's meanLinkDelay against the tester's own
TE_AGREE_NS = 50_000             # time errors seen three ways must agree within this
RESP_TURNAROUND_MAX_S = 0.010    # Pdelay_Req in to Pdelay_Resp out (Avnu)

MASTER_WITHIN_S = 15.0           # alone, the DUT becomes grandmaster within
SLAVE_WITHIN_S = 15.0            # with a better grandmaster, SLAVE within
RESELECT_WITHIN_S = 6.0          # a better/worse Announce changes the role within
ASCAP_DROP_WITHIN_S = 4.0        # asCapable drops within (except lost responses)
ASCAP_BACK_WITHIN_S = 6.0        # and comes back within
LINK_BACK_WITHIN_S = 10.0        # SLAVE again after link-up within (G1.1)

ANNOUNCE_TIMEOUT_S = (2.5, 5.0)  # 3 announce intervals of 1 s
SYNC_TIMEOUT_S = (0.25, 1.2)     # 3 sync intervals of 125 ms

RSYNC_DEFAULT_S = 0.9 * 0.125    # 90 % of the incoming Sync interval
IDLE_CPU_MAX = 0.02              # flexptpd idle CPU share (G1.3)
TERM_WITHIN_S = 1.0

# the suite's DUT configuration on top of the daemon's gPTP preset
VETH_CONFIG = [
    "ptp gptp nbthresh %d" % VETH_NBTHRESH_NS,
]

# "ptp rsync rate PCT" is read as PCT of the incoming Sync *interval*
# (ticket G5: "the default is 90 % of the incoming Sync interval")
RSYNC_RATE_IS_INTERVAL_PCT = True


# ---- helpers ----------------------------------------------------------------

def within(x: float, nominal: float, tol: float) -> bool:
    return abs(x - nominal) <= tol * nominal


def fmt_s(xs: list) -> str:
    if not xs:
        return "none"

    return "median %.4f s, min %.4f, max %.4f, n %d" % (median(xs), min(xs), max(xs), len(xs))


class StateTracker:
    """Samples the DUT's status every 20 ms: (realtime ns, Status)."""

    def __init__(self, dut: Dut):
        self.dut = dut
        self.samples = []

        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        while not self._stop.is_set():
            st = self.dut.status()

            if st is not None:
                self.samples.append((now_ns(), st))

            self._stop.wait(0.02)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)

    def first(self, pred: Callable, since: int = 0) -> Optional[int]:
        for t, st in list(self.samples):
            if t >= since and pred(st):
                return t

        return None

    def spans(self, state: int) -> list:
        """(start, end) realtime spans the port spent in `state`."""
        out = []
        start = None

        for t, st in list(self.samples):
            if st.portState == state and start is None:
                start = t
            elif st.portState != state and start is not None:
                out.append((start, t))
                start = None

        if start is not None:
            out.append((start, now_ns()))

        return out


class Context:
    def __init__(self, binary: str, dut_if: str, tester_if: str, outdir: str):
        self.binary = binary
        self.dut_if = dut_if
        self.tester_if = tester_if
        self.outdir = outdir

        self._duts = []
        self._roles = []
        self.ep: Optional[Endpoint] = None

    # one tester endpoint, one pcap per check
    def begin(self, check_name: str) -> None:
        self.ep = Endpoint(self.tester_if, pcap_path="%s/%s.pcap" % (self.outdir, check_name))
        self.check_name = check_name

    def end(self) -> None:
        for r in self._roles:
            try:
                r.stop()
            except Exception:
                pass

        for d in self._duts:
            d.stop()

        self._roles.clear()
        self._duts.clear()

        if self.ep:
            self.ep.close()
            self.ep = None

    def role(self, r):
        self._roles.append(r)
        return r

    def dut(self, config: Optional[list] = None, **kw) -> Dut:
        name = "%s-%d" % (self.check_name, len(self._duts))

        d = Dut(self.binary, self.dut_if, self.outdir, name=name, config=VETH_CONFIG + (config or []), **kw)
        d.start()

        self._duts.append(d)
        return d

    def responder(self, **kw) -> PdelayResponder:
        return self.role(PdelayResponder(self.ep, **kw))

    def gm(self, **kw) -> GrandMaster:
        return self.role(GrandMaster(self.ep, **kw))

    def tracker(self, dut: Dut) -> StateTracker:
        return self.role(StateTracker(dut))

    def tester_link(self, up: bool) -> None:
        subprocess.run(["ip", "link", "set", self.tester_if, "up" if up else "down"], check=True)

    def dut_frames(self, dut_id: bytes, **kw) -> list:
        return self.ep.frames(source=dut_id, **kw)


def is_state(s):
    return lambda st: st.portState == s


def wait_master(dut: Dut, timeout: float = MASTER_WITHIN_S):
    return dut.wait_status(is_state(MASTER), timeout)


def wait_slave_to(dut: Dut, gm_id: bytes, timeout: float = SLAVE_WITHIN_S):
    return dut.wait_status(lambda st: st.portState == SLAVE and st.gm_identity == gm_id, timeout)


# ---- G1: the Linux platform ---------------------------------------------------

def g1_link(ctx: Context) -> list:
    """G1.1 a link drop does not stall the stack; SLAVE again within 10 s."""
    ctx.responder()
    gm = ctx.gm()
    dut = ctx.dut()

    if wait_slave_to(dut, gm.identity) is None:
        return [("G1.1", False, "never SLAVE to the tester: %r" % dut.status())]

    before = dut.status()
    ctx.tester_link(False)
    t_down = time.monotonic()

    down = dut.wait_status(lambda st: not st.linkUp, 2.0)
    time.sleep(max(0.0, 2.0 - (time.monotonic() - t_down)))

    during = dut.status()
    ctx.tester_link(True)
    t_up = time.monotonic()

    back = wait_slave_to(dut, gm.identity, LINK_BACK_WITHIN_S)
    took = time.monotonic() - t_up

    alive_heartbeat = during is not None and during.updates > before.updates

    ok = dut.alive() and down is not None and alive_heartbeat and back is not None

    detail = "link down seen %s, status still updating during the drop %s, SLAVE again after %.1f s (limit %.0f)" % (
        down is not None,
        alive_heartbeat,
        took if back else float("inf"),
        LINK_BACK_WITHIN_S,
    )

    return [("G1.1", ok, detail)]


def g1_daemon(ctx: Context) -> list:
    """G1.3 idle CPU and SIGTERM; G1.4 no IPv4 address on the interface."""
    out = []

    addr = subprocess.run(["ip", "-4", "-o", "addr", "show", "dev", ctx.dut_if], capture_output=True, text=True).stdout
    ctx.responder()
    dut = ctx.dut()

    st = dut.wait_status(lambda s: s.linkUp, 5.0)
    out.append(("G1.4", addr.strip() == "" and st is not None, "no IPv4 on %s: %s, running: %r" % (ctx.dut_if, addr.strip() == "", st)))

    # idle: settled, no grandmaster but ourselves
    time.sleep(3.0)
    c0 = dut.cpu_seconds()
    time.sleep(5.0)
    share = (dut.cpu_seconds() - c0) / 5.0

    out.append(("G1.3a", share <= IDLE_CPU_MAX, "CPU %.2f %% over 5 s with stdin at EOF (limit %.0f %%)" % (share * 100, IDLE_CPU_MAX * 100)))

    took = dut.stop()
    out.append(("G1.3b", took is not None and took <= TERM_WITHIN_S, "SIGTERM: exited after %s s (limit %.1f)" % (
        "%.2f" % took if took is not None else "never", TERM_WITHIN_S)))

    out.append(("G1.3c", True, "SCHED_FIFO not checked here: a user namespace cannot grant it (run -P on the board)"))
    return out


# ---- G2: peer delay ------------------------------------------------------------

def g2_cadence(ctx: Context) -> list:
    """G2.1 Pdelay_Req every 1 s +/-10 % in LISTENING, MASTER and SLAVE."""
    ctx.responder()
    dut = ctx.dut()
    track = ctx.tracker(dut)
    dut_id = dut.identity()

    if wait_master(dut) is None:
        return [("G2.1", False, "never MASTER alone: %r" % dut.status())]

    time.sleep(5.0)

    gm = ctx.gm()

    if wait_slave_to(dut, gm.identity) is None:
        return [("G2.1", False, "never SLAVE to the tester: %r" % dut.status())]

    time.sleep(6.0)

    out = []

    for name, state in (("LISTENING", LISTENING), ("MASTER", MASTER), ("SLAVE", SLAVE)):
        reqs = []

        for a, b in track.spans(state):
            reqs += [ctx.dut_frames(dut_id, types=gptp.PDELAY_REQ, since=a, until=b)]

        ivs = []
        for span in reqs:
            ivs += intervals(span)

        n = sum(len(s) for s in reqs)

        if name == "LISTENING" and n < 2:
            out.append(("G2.1-" + name, n >= 1, "%d request(s) while LISTENING (too short a state to time)" % n))
            continue

        ok = bool(ivs) and all(within(x, 1.0, INTERVAL_TOL) for x in ivs)
        out.append(("G2.1-" + name, ok, fmt_s(ivs)))

    return out


def g2_rate_delay(ctx: Context) -> list:
    """G2.2 neighborRateRatio within 1 ppm of the known rates; meanLinkDelay sane."""
    responder_ppm = 50.0
    dut_drift_ppb = 20_000

    ctx.responder(rate_ppm=responder_ppm)
    dut = ctx.dut(drift_ppb=dut_drift_ppb)

    if dut.wait_status(lambda st: st.asCapable, 10.0) is None:
        return [("G2.2", False, "never asCapable: %r" % dut.status())]

    # a window for the ratio: the DUT is grandmaster of itself, its clock free-running
    time.sleep(15.0)
    st = dut.status()

    expected = (1 + responder_ppm * 1e-6) / (1 + dut_drift_ppb * 1e-9)
    err_ppm = (st.neighborRateRatio - expected) * 1e6

    out = [("G2.2a", abs(err_ppm) <= NRR_TOL_PPM, "neighborRateRatio %.9f, expected %.9f: %+.3f ppm (limit %.1f)" % (
        st.neighborRateRatio, expected, err_ppm, NRR_TOL_PPM))]

    # the tester measures the same link through the DUT's responder
    init = PdelayInitiator(ctx.ep)
    mine = [m["link_delay_ns"] for m in (init.measure() for _ in range(5)) if m and "link_delay_ns" in m]

    tester_mld = statistics.median(mine) if mine else float("nan")
    ok = 0 <= st.meanLinkDelay_ns <= MLD_MAX_NS and mine and abs(st.meanLinkDelay_ns - tester_mld) <= MLD_AGREE_NS

    out.append(("G2.2b", bool(ok), "meanLinkDelay %d ns (sane: 0..%d), tester's own %.0f ns (agree within %d)" % (
        st.meanLinkDelay_ns, MLD_MAX_NS, tester_mld, MLD_AGREE_NS)))

    return out


def _ascap_cycle(dut: Dut, cause: Callable, cure: Callable, drop_within: float) -> tuple:
    """(dropped after s, back after s): asCapable goes away with cause(), returns with cure()."""
    t0 = time.monotonic()
    cause()

    dropped = dut.wait_status(lambda st: not st.asCapable, drop_within)
    t_drop = time.monotonic() - t0

    t1 = time.monotonic()
    cure()

    back = dut.wait_status(lambda st: st.asCapable, ASCAP_BACK_WITHIN_S)
    t_back = time.monotonic() - t1

    return (t_drop if dropped else None, t_back if back else None)


def _fmt_cycle(r: tuple) -> str:
    d, b = r
    return "dropped after %s, back after %s" % (
        "%.1f s" % d if d is not None else "never",
        "%.1f s" % b if b is not None else "never",
    )


def g2_ascapable(ctx: Context) -> list:
    """G2.3 asCapable drops on each cause and comes back once it goes."""
    resp = ctx.responder()
    dut = ctx.dut()

    if dut.wait_status(lambda st: st.asCapable, 10.0) is None:
        return [("G2.3", False, "never asCapable: %r" % dut.status())]

    dut_id = dut.identity()
    out = []

    # a: neighborPropDelay above the threshold
    def slow():
        resp.extra_delay_ns = 2 * VETH_NBTHRESH_NS

    def fast():
        resp.extra_delay_ns = 0

    r = _ascap_cycle(dut, slow, fast, ASCAP_DROP_WITHIN_S)
    out.append(("G2.3a", None not in r, "link delay above neighborPropDelayThresh: " + _fmt_cycle(r)))

    # b: lost responses: still asCapable after 9 lost in a row, not after 10
    time.sleep(2.0)
    resp.drop_next = 9

    deadline = time.monotonic() + 12.0
    dropped_early = False

    while resp.drop_next > 0 and time.monotonic() < deadline:
        st = dut.status()

        if st is not None and not st.asCapable:
            dropped_early = True

        time.sleep(0.05)

    still = not dropped_early and resp.drop_next == 0

    # the 10th in a row, before the next request comes in
    def lose_one():
        resp.drop_next = 1

    def answer():
        resp.drop_next = 0

    r = _ascap_cycle(dut, lose_one, answer, 2.5)
    out.append(("G2.3b", still and None not in r, "after 9 lost still asCapable: %s; the 10th: %s" % (still, _fmt_cycle(r))))

    # c: two responders to one request
    time.sleep(2.0)

    def twin():
        resp.twin = bytes.fromhex("020000fffe0000ee")

    def single():
        resp.twin = None

    r = _ascap_cycle(dut, twin, single, ASCAP_DROP_WITHIN_S)
    out.append(("G2.3c", None not in r, "two responders: " + _fmt_cycle(r)))

    # d: our own identity answering
    time.sleep(2.0)
    mine = resp.identity

    def loop():
        resp.identity = dut_id

    def unloop():
        resp.identity = mine

    r = _ascap_cycle(dut, loop, unloop, ASCAP_DROP_WITHIN_S)
    out.append(("G2.3d", None not in r, "the DUT's own identity answering: " + _fmt_cycle(r)))

    return out


def g2_responder(ctx: Context) -> list:
    """G2.4 the DUT's Pdelay_Resp: within 10 ms, the right fields."""
    ctx.responder()
    dut = ctx.dut()

    # measured once the DUT is up and running its port: a request that lands
    # while it is still starting up is not a responder fault
    dut.wait_status(lambda st: st.asCapable, 10.0)
    dut_id = dut.identity()

    init = PdelayInitiator(ctx.ep)
    errors = []
    turns = []

    for _ in range(8):
        m = init.measure()
        time.sleep(0.2)

        if m is None:
            errors.append("no Pdelay_Resp")
            continue

        if m.get("fu") is None:
            errors.append("no Pdelay_Resp_Follow_Up")
            continue

        r = m["resp"].msg
        f = m["fu"].msg
        turns.append(m["turnaround_ns"] / 1e9)

        checks = {
            "majorSdoId 1": r.hdr.major_sdo_id == 1 and f.hdr.major_sdo_id == 1,
            "two-step flag": r.hdr.two_step,
            "logMessageInterval 0x7F": (r.hdr.log_interval & 0xFF) == 0x7F and (f.hdr.log_interval & 0xFF) == 0x7F,
            "requestingPortIdentity": r.requesting == init.src and f.requesting == init.src,
            "sequenceId echoed": r.hdr.seq == f.hdr.seq == init.seq,
            "domain 0": r.hdr.domain == 0 and f.hdr.domain == 0,
            "controlField 5": r.hdr.control == 5 and f.hdr.control == 5,
            "source is the DUT": r.hdr.source.clock == dut_id and f.hdr.source.clock == dut_id,
            "t3 >= t2": m["t3"] >= m["t2"],
        }

        errors += [k for k, ok in checks.items() if not ok]

    late = [t for t in turns if t > RESP_TURNAROUND_MAX_S]

    ok_fields = not errors
    ok_time = bool(turns) and not late

    return [
        ("G2.4a", ok_time, "Pdelay_Req out to Pdelay_Resp in: %s (limit %.0f ms)" % (fmt_s(turns), RESP_TURNAROUND_MAX_S * 1e3)),
        ("G2.4b", ok_fields, "fields: " + ("all as expected over %d exchanges" % len(turns) if ok_fields else ", ".join(sorted(set(errors))))),
    ]


# ---- G3: BMCA, path trace, sync, grandmaster -----------------------------------

FIELDS = ("priority1", "clock_class", "clock_accuracy", "variance", "priority2", "identity")


def _dut_announce(ctx: Context, dut_id: bytes, timeout: float = 3.0):
    since = now_ns()
    return ctx.ep.wait_frame(lambda c: c.type == gptp.ANNOUNCE and c.msg.hdr.source.clock == dut_id, timeout, since)


def g3_bmca(ctx: Context) -> list:
    """G3.1 the role follows each field of the priority vector; loops and
    stepsRemoved 255 are discarded."""
    ctx.responder()
    dut = ctx.dut()

    if wait_master(dut) is None:
        return [("G3.1", False, "never MASTER alone: %r" % dut.status())]

    dut_id = dut.identity()
    ann = _dut_announce(ctx, dut_id)

    if ann is None:
        return [("G3.1", False, "no Announce from the DUT as MASTER")]

    v = ann.msg
    tester_gm = bytes.fromhex("020000fffe0000a1")
    out = []

    gm = ctx.gm(
        identity=tester_gm,
        priority1=v.priority1,
        clock_class=v.clock_class,
        clock_accuracy=v.clock_accuracy,
        variance=v.variance,
        priority2=v.priority2,
    )
    gm.send_announce = False
    gm.send_sync = False

    dut_n = gptp.clock_id_int(dut_id)

    def set_vector(field: str, delta: int) -> bytes:
        gm.priority1 = v.priority1
        gm.clock_class = v.clock_class
        gm.clock_accuracy = v.clock_accuracy
        gm.variance = v.variance
        gm.priority2 = v.priority2
        gm.gm_identity = tester_gm
        gm.path_trace = None

        if field == "identity":
            # the same systemIdentity but for the identity: decided by it
            gm.gm_identity = (dut_n + delta).to_bytes(8, "big")
        else:
            setattr(gm, field, getattr(v, field) + delta)

        return gm.gm_identity

    for field in FIELDS:
        for delta, want in ((-1, SLAVE), (+1, MASTER)):
            if field != "identity" and not (0 <= getattr(v, field) + delta <= (0xFFFF if field == "variance" else 0xFF)):
                out.append(("G3.1-%s%+d" % (field, delta), True, "skipped: %s is %d, no room" % (field, getattr(v, field))))
                continue

            gm_id = set_vector(field, delta)
            gm.send_announce = True
            gm.send_sync = True

            if want == SLAVE:
                st = dut.wait_status(lambda s: s.portState == SLAVE and s.gm_identity == gm_id, RESELECT_WITHIN_S)
            else:
                st = dut.wait_status(lambda s: s.portState == MASTER and s.gm_identity == dut_id, RESELECT_WITHIN_S)

            out.append((
                "G3.1-%s%+d" % (field, delta),
                st is not None,
                "%s %s by one: expected %s, got %r" % (field, "better" if delta < 0 else "worse", "SLAVE" if want == SLAVE else "MASTER", dut.status()),
            ))

            # back to MASTER before the next field
            gm.send_announce = False
            gm.send_sync = False
            wait_master(dut, 6.0)

    # discards: a much better vector, but looped through us, or 255 steps away
    for name, kw in (("path trace holds the DUT", {"path_trace": [tester_gm, dut_id]}),
                     ("stepsRemoved 255", {"steps_removed": 255})):
        set_vector("priority1", -100)
        gm.path_trace = kw.get("path_trace")
        gm.steps_removed = kw.get("steps_removed", 0)
        gm.send_announce = True
        gm.send_sync = True

        moved = dut.wait_status(lambda s: s.portState != MASTER or s.gm_identity != dut_id, 4.0)
        out.append(("G3.1-discard", moved is None, "%s: %s" % (name, "ignored, still MASTER" if moved is None else "taken: %r" % moved)))

        gm.send_announce = False
        gm.send_sync = False
        gm.steps_removed = 0
        wait_master(dut, 6.0)

    return out


def g3_timeouts(ctx: Context) -> list:
    """G3.2 announce receipt timeout and sync receipt timeout: reselection
    after 3 intervals."""
    ctx.responder()
    gm = ctx.gm()
    dut = ctx.dut()
    out = []

    if wait_slave_to(dut, gm.identity) is None:
        return [("G3.2", False, "never SLAVE to the tester: %r" % dut.status())]

    time.sleep(2.0)

    # announce receipt timeout: Announce stops, Sync goes on
    before = dut.status().announceTimeouts
    t0 = time.monotonic()
    gm.send_announce = False

    st = dut.wait_status(lambda s: s.announceTimeouts > before or s.portState != SLAVE, ANNOUNCE_TIMEOUT_S[1] + 1)
    took = time.monotonic() - t0

    ok = st is not None and ANNOUNCE_TIMEOUT_S[0] <= took <= ANNOUNCE_TIMEOUT_S[1]
    out.append(("G3.2a", ok, "Announce stopped: reselected after %.2f s (expected %.1f..%.1f), %r" % (
        took, ANNOUNCE_TIMEOUT_S[0], ANNOUNCE_TIMEOUT_S[1], st)))

    gm.send_announce = True

    if wait_slave_to(dut, gm.identity) is None:
        out.append(("G3.2b", False, "never SLAVE again: %r" % dut.status()))
        return out

    time.sleep(2.0)

    # sync receipt timeout: Sync stops, Announce goes on
    before = dut.status().syncTimeouts
    t0 = time.monotonic()
    gm.send_sync = False

    st = dut.wait_status(lambda s: s.syncTimeouts > before or s.portState != SLAVE, SYNC_TIMEOUT_S[1] + 1, poll=0.01)
    took = time.monotonic() - t0

    ok = st is not None and SYNC_TIMEOUT_S[0] <= took <= SYNC_TIMEOUT_S[1]
    out.append(("G3.2b", ok, "Sync stopped: reselected after %.3f s (expected %.2f..%.2f), %r" % (
        took, SYNC_TIMEOUT_S[0], SYNC_TIMEOUT_S[1], st)))

    gm.send_sync = True
    return out


def g3_foreign_sync(ctx: Context) -> list:
    """G3.3 Sync/Follow_Up from another port identity do not move the clock."""
    ctx.responder()
    gm = ctx.gm(time_offset_ns=5_000_000)
    gm.sync_source = gptp.PortIdentity(bytes.fromhex("020000fffe0000f0"), 3)

    dut = ctx.dut(offset_ns=0, drift_ppb=0)
    since = time.monotonic()

    dut.wait_status(lambda s: s.gm_identity == gm.identity, SLAVE_WITHIN_S)
    time.sleep(6.0)

    errs = dut.vclock_errors(since)
    st = dut.status()

    moved = [e for e in errs if abs(e) > 1_000_000]
    ok = bool(errs) and not moved and st is not None and st.syncRx == 0

    out = [("G3.3", ok, "foreign Sync 5 ms ahead for 6 s: clock error max %s ns, syncRx %s" % (
        max((abs(e) for e in errs), default="n/a"), st.syncRx if st else "n/a"))]

    # positive control: the same Syncs from the announced port are used
    gm.sync_source = None
    taken = dut.wait_status(lambda s: s.syncRx > 0, 3.0)
    out.append(("G3.3-control", taken is not None, "Sync from the announcing port: syncRx %s" % (taken.syncRx if taken else 0)))

    return out


def g3_grandmaster(ctx: Context) -> list:
    """G3.4 as grandmaster: Announce, Sync and Follow_Up fields; nothing
    sent once not asCapable."""
    resp = ctx.responder()
    dut = ctx.dut()

    if wait_master(dut) is None:
        return [("G3.4", False, "never MASTER alone: %r" % dut.status())]

    dut_id = dut.identity()
    t0 = now_ns()
    time.sleep(6.0)

    anns = ctx.dut_frames(dut_id, types=gptp.ANNOUNCE, since=t0)
    syncs = ctx.dut_frames(dut_id, types=gptp.SYNC, since=t0, domain=0)
    fus = ctx.dut_frames(dut_id, types=gptp.FOLLOW_UP, since=t0, domain=0)
    out = []

    errors = []

    for c in anns:
        a = c.msg
        checks = {
            "majorSdoId 1": a.hdr.major_sdo_id == 1,
            "priority1 248": a.priority1 == 248,
            "clockClass 248": a.clock_class == 248,
            "grandmasterIdentity = sourcePortIdentity (byte order)": a.gm_identity == dut_id,
            "stepsRemoved 0": a.steps_removed == 0,
            "path trace [DUT]": a.path_trace == [dut_id],
            "ptpTimescale": bool(a.hdr.flags & gptp.FLAG_PTP_TIMESCALE),
            "controlField 5": a.hdr.control == 5,
        }
        errors += [k for k, ok in checks.items() if not ok]

    out.append(("G3.4a", bool(anns) and not errors, "%d Announce: %s; interval %s" % (
        len(anns), "fields as expected" if not errors else ", ".join(sorted(set(errors))), fmt_s(intervals(anns)))))

    ann_ok = len(anns) >= 2 and all(within(x, 1.0, INTERVAL_TOL) for x in intervals(anns))
    out.append(("G3.4b", ann_ok, "Announce interval " + fmt_s(intervals(anns))))

    errors = []
    by_seq = {c.msg.hdr.seq: c for c in fus}

    for c in syncs:
        s = c.msg
        f = by_seq.get(s.hdr.seq)

        checks = {
            "two-step": s.hdr.two_step,
            "logMessageInterval -3": s.hdr.log_interval == -3,
            "Sync controlField 0": s.hdr.control == 0,
            "Follow_Up for every Sync": f is not None,
            "Follow_Up information TLV": f is not None and f.msg.info is not None,
            "Follow_Up controlField 2": f is not None and f.msg.hdr.control == 2,
            "preciseOriginTimestamp near the Sync": f is not None and abs(f.msg.precise_origin - c.ts) < 1_000_000,
        }
        errors += [k for k, ok in checks.items() if not ok]

    out.append(("G3.4c", bool(syncs) and not errors, "%d Sync: %s" % (
        len(syncs), "fields as expected" if not errors else ", ".join(sorted(set(errors))))))

    sync_ok = len(syncs) >= 10 and abs(median(intervals(syncs)) - 0.125) <= INTERVAL_TOL * 0.125
    out.append(("G3.4d", sync_ok, "Sync interval " + fmt_s(intervals(syncs))))

    # not asCapable: no Sync, no Announce
    resp.extra_delay_ns = 2 * VETH_NBTHRESH_NS
    gone = dut.wait_status(lambda s: not s.asCapable, ASCAP_DROP_WITHIN_S)

    if gone is None:
        out.append(("G3.4e", False, "asCapable never dropped: %r" % dut.status()))
        return out

    time.sleep(0.5)
    t1 = now_ns()
    time.sleep(3.0)

    sent = ctx.dut_frames(dut_id, types=(gptp.SYNC, gptp.ANNOUNCE), since=t1, domain=0)
    out.append(("G3.4e", not sent, "while not asCapable: %d Sync/Announce sent in 3 s" % len(sent)))

    return out


# ---- G4: signaling ---------------------------------------------------------------

def _rates(ctx: Context, dut_id: bytes, seconds: float) -> dict:
    t0 = now_ns()
    time.sleep(seconds)

    def iv(t):
        return intervals(ctx.dut_frames(dut_id, types=t, since=t0, domain=0))

    return {
        "pdelay": iv(gptp.PDELAY_REQ),
        "sync": iv(gptp.SYNC),
        "announce": iv(gptp.ANNOUNCE),
    }


def _match(r: dict, want: dict) -> tuple:
    bad = []

    for k, nominal in want.items():
        xs = r[k]

        if nominal is None:
            if xs:
                bad.append("%s still sent (%s)" % (k, fmt_s(xs)))
        elif not xs or not within(median(xs), nominal, INTERVAL_TOL):
            bad.append("%s %s, expected %.3f s" % (k, fmt_s(xs), nominal))

    return (not bad, "; ".join(bad) if bad else "as requested")


def g4_signaling(ctx: Context) -> list:
    """G4.1 message interval requests set the DUT's intervals; G4.2 a malformed
    or foreign TLV changes nothing."""
    ctx.responder()
    dut = ctx.dut()

    if wait_master(dut) is None:
        return [("G4.1", False, "never MASTER alone: %r" % dut.status())]

    dut_id = dut.identity()
    out = []
    seq = [0]

    def ask(ldi, tsi, ai, extra=b""):
        seq[0] += 1
        req = None if ldi is None else gptp.MsgIntervalReqTlv(ldi, tsi, ai)
        send_signaling(ctx.ep, req, seq=seq[0], extra=extra)
        time.sleep(0.3)

    ask(1, -2, 1)
    ok, d = _match(_rates(ctx, dut_id, 7.0), {"pdelay": 2.0, "sync": 0.25, "announce": 2.0})
    out.append(("G4.1-set", ok, "linkDelay 1, timeSync -2, announce 1: " + d))

    ask(gptp.INTERVAL_NO_CHANGE, gptp.INTERVAL_NO_CHANGE, gptp.INTERVAL_NO_CHANGE)
    ok, d = _match(_rates(ctx, dut_id, 5.0), {"pdelay": 2.0, "sync": 0.25, "announce": 2.0})
    out.append(("G4.1-no-change", ok, "-128 everywhere: " + d))

    ask(gptp.INTERVAL_INITIAL, gptp.INTERVAL_INITIAL, gptp.INTERVAL_INITIAL)
    ok, d = _match(_rates(ctx, dut_id, 4.0), {"pdelay": 1.0, "sync": 0.125, "announce": 1.0})
    out.append(("G4.1-initial", ok, "126 everywhere: " + d))

    ask(gptp.INTERVAL_STOP, gptp.INTERVAL_NO_CHANGE, gptp.INTERVAL_NO_CHANGE)
    ok, d = _match(_rates(ctx, dut_id, 4.0), {"pdelay": None, "sync": 0.125})
    out.append(("G4.1-stop", ok, "127 for linkDelay: " + d))

    ask(gptp.INTERVAL_INITIAL, gptp.INTERVAL_NO_CHANGE, gptp.INTERVAL_NO_CHANGE)
    time.sleep(1.5)

    # G4.2: a request in a foreign organization, then one with a bad length
    foreign = gptp.Tlv(
        gptp.TLV_ORGANIZATION_EXTENSION,
        bytes.fromhex("001122") + (2).to_bytes(3, "big") + bytes([1, 0xFE, 1, 6, 0, 0]),
    ).pack()

    short = gptp.Tlv(
        gptp.TLV_ORGANIZATION_EXTENSION,
        gptp.ORG_IEEE_8021 + (2).to_bytes(3, "big") + bytes([1, 0xFE]),
    ).pack()

    ask(None, None, None, extra=foreign)
    ask(None, None, None, extra=short)

    ok, d = _match(_rates(ctx, dut_id, 4.0), {"pdelay": 1.0, "sync": 0.125, "announce": 1.0})
    out.append(("G4.2", ok and dut.alive(), "foreign organization and short TLV: " + d))

    return out


# ---- G5: reverse sync -------------------------------------------------------------

def _rsync_frames(ctx: Context, dut_id: bytes, since: int) -> list:
    return [c for c in ctx.dut_frames(dut_id, types=gptp.SYNC, since=since) if c.domain != 0]


def g5_rsync_messages(ctx: Context) -> list:
    """G5.1 off by default; when on: its domain only, its rate, two-step, the TLV."""
    out = []

    ctx.responder()
    gm = ctx.gm()

    # off by default
    dut = ctx.dut()
    wait_slave_to(dut, gm.identity)
    dut_id = dut.identity()
    t0 = now_ns()
    time.sleep(4.0)

    others = [c for c in ctx.dut_frames(dut_id, since=t0) if c.domain != 0]
    st = dut.status()
    out.append(("G5.1-default-off", not others and st is not None and not st.rsyncEnabled,
                "%d frames on other domains, rsyncEnabled %s" % (len(others), st.rsyncEnabled if st else "n/a")))
    dut.stop()

    variants = [
        ("default", ["ptp rsync on", "ptp rsync domain 1"], RSYNC_DEFAULT_S),
        ("interval 200 ms", ["ptp rsync on", "ptp rsync domain 1", "ptp rsync interval 200"], 0.200),
    ]

    if RSYNC_RATE_IS_INTERVAL_PCT:
        variants.append(("rate 50 %", ["ptp rsync on", "ptp rsync domain 1", "ptp rsync rate 50"], 0.5 * 0.125))

    for name, cfg, nominal in variants:
        dut = ctx.dut(config=cfg)

        if wait_slave_to(dut, gm.identity) is None:
            out.append(("G5.1-" + name, False, "never SLAVE: %r" % dut.status()))
            dut.stop()
            continue

        dut_id = dut.identity()
        t0 = now_ns()
        time.sleep(5.0)

        rs = _rsync_frames(ctx, dut_id, t0)
        fus = {c.msg.hdr.seq: c for c in ctx.dut_frames(dut_id, types=gptp.FOLLOW_UP, since=t0) if c.domain == 1}
        dom0 = ctx.dut_frames(dut_id, types=(gptp.SYNC, gptp.FOLLOW_UP), since=t0, domain=0)

        errors = []

        for c in rs:
            f = fus.get(c.msg.hdr.seq)

            checks = {
                "domain 1": c.domain == 1,
                "two-step": c.msg.hdr.two_step,
                "Follow_Up paired": f is not None,
                "Follow_Up information TLV": f is not None and f.msg.info is not None,
            }
            errors += [k for k, ok in checks.items() if not ok]

        ivs = intervals(rs)
        rate_ok = len(rs) >= 10 and within(median(ivs), nominal, RSYNC_INTERVAL_TOL)

        ok = rate_ok and not errors and not dom0
        out.append(("G5.1-" + name, ok, "%d reverse Sync, interval %s (expected %.4f +/-%d %%), %s, %d Sync/Follow_Up on domain 0" % (
            len(rs), fmt_s(ivs), nominal, RSYNC_INTERVAL_TOL * 100,
            "fields as expected" if not errors else ", ".join(sorted(set(errors))), len(dom0))))

        dut.stop()

    return out


def g5_rsync_error(ctx: Context) -> list:
    """G5.2 the time error from reverse sync agrees with the DUT's own; G5.3
    reverse sync goes on in holdover."""
    out = []

    ctx.responder()
    gm = ctx.gm()
    rx = ctx.role(ReverseSyncReceiver(ctx.ep, 1))

    dut = ctx.dut(config=["ptp rsync on", "ptp rsync domain 1"], offset_ns=3_000_000, drift_ppb=30_000)

    if wait_slave_to(dut, gm.identity) is None:
        return [("G5.2", False, "never SLAVE: %r" % dut.status())]

    locked = dut.wait_status(lambda s: s.locked, 30.0)
    time.sleep(5.0)

    t0 = now_ns()
    m0 = time.monotonic()
    reported = []

    for _ in range(40):
        st = dut.status()

        if st is not None:
            reported.append(st.timeError_ns)

        time.sleep(0.25)

    link = dut.status().meanLinkDelay_ns
    egress = rx.time_errors(link, since=t0)
    truth = dut.vclock_errors(m0)

    if not egress or not truth or not reported:
        return [("G5.2", False, "egress samples %d, vclock-error lines %d (is -v printing?), status samples %d" % (
            len(egress), len(truth), len(reported)))]

    e = statistics.mean(egress)
    r = statistics.mean(reported)
    g = statistics.mean(truth)

    ok = abs(e - r) <= TE_AGREE_NS and abs(e - g) <= TE_AGREE_NS
    out.append(("G5.2", ok, "time error by reverse sync %.0f ns (n %d, sd %.0f), reported %.0f, virtual clock %.0f; agree within %d; locked %s" % (
        e, len(egress), statistics.pstdev(egress), r, g, TE_AGREE_NS, locked is not None)))

    # holdover: the grandmaster stops, reverse sync does not
    gm.send_sync = False
    gm.send_announce = False

    time.sleep(1.0)
    t1 = now_ns()
    time.sleep(5.0)

    held = [s for s in rx.samples if s[0] >= t1]
    expected = 5.0 / RSYNC_DEFAULT_S

    drift = ""
    if len(held) >= 2:
        pts = [((s[0] - t1) / 1e9, s[1] + link - s[0]) for s in held]
        drift = ", drift %+.3f ppm" % (slope(pts) / 1e3)

    out.append(("G5.3", len(held) >= 0.8 * expected, "grandmaster gone: %d reverse Sync in 5 s (expected about %.0f)%s" % (
        len(held), expected, drift)))

    return out


# ---- the registry ----------------------------------------------------------------

CHECKS = [
    ("g1-link", g1_link),
    ("g1-daemon", g1_daemon),
    ("g2-cadence", g2_cadence),
    ("g2-rate-delay", g2_rate_delay),
    ("g2-ascapable", g2_ascapable),
    ("g2-responder", g2_responder),
    ("g3-bmca", g3_bmca),
    ("g3-timeouts", g3_timeouts),
    ("g3-foreign-sync", g3_foreign_sync),
    ("g3-grandmaster", g3_grandmaster),
    ("g4-signaling", g4_signaling),
    ("g5-rsync-messages", g5_rsync_messages),
    ("g5-rsync-error", g5_rsync_error),
]
