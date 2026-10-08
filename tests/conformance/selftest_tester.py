# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""selftest_tester.py - the tester against itself, with no DUT: one endpoint
on each end of the veth pair. It proves the roles and the arithmetic the
checks rely on: link delay, a responder's clock rate, the egress time error,
signaling and Announce parsing.

usage (inside the namespace of run.sh):  selftest_tester.py IF_A IF_B
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gptp                                   # noqa: E402
from tester import (Endpoint, GrandMaster, PdelayInitiator, PdelayResponder,  # noqa: E402
                    ReverseSyncReceiver, median, send_signaling, slope)

EXTRA_DELAY_NS = 1_000
RATE_PPM = 50.0
GM_OFFSET_NS = 250_000

LINK_MAX_NS = 50_000          # veth with software timestamps
RATE_TOL_PPM = 5.0            # 20 exchanges over 2 s, software timestamps
TE_TOL_NS = 30_000


def main() -> int:
    if_a, if_b = sys.argv[1], sys.argv[2]

    a = Endpoint(if_a)
    b = Endpoint(if_b)

    failures = 0

    def check(name: str, ok: bool, detail: str) -> None:
        nonlocal failures

        print(("ok    " if ok else "FAIL  ") + name + ": " + detail)

        if not ok:
            failures += 1

    try:
        # ---- transmit timestamps ----
        t = a.send(gptp.build_pdelay_req(a.port(), 1))
        now = time.clock_gettime_ns(time.CLOCK_REALTIME)
        check("software transmit timestamp", 0 <= now - t < 50_000_000, "%d ns before now" % (now - t))

        # ---- pdelay: a responder with an extra delay and a fast clock ----
        resp = PdelayResponder(a, extra_delay_ns=EXTRA_DELAY_NS, rate_ppm=RATE_PPM)
        init = PdelayInitiator(b)

        ms = []
        for _ in range(20):
            m = init.measure()

            if m and "link_delay_ns" in m:
                ms.append(m)

            time.sleep(0.1)

        delays = [m["link_delay_ns"] for m in ms]
        d = median(delays) if delays else float("nan")

        check("link delay = wire + extra", len(ms) >= 18 and EXTRA_DELAY_NS <= d <= EXTRA_DELAY_NS + LINK_MAX_NS,
              "%d exchanges, median %.0f ns (extra %d)" % (len(ms), d, EXTRA_DELAY_NS))

        # the responder's clock against ours: the slope of t3 over t4
        pts = [(m["t4"] / 1e9, (m["t3"] - m["t4"]) / 1e3) for m in ms]
        rate = slope(pts) if len(pts) >= 2 else float("nan")

        check("responder clock rate", abs(rate - RATE_PPM) <= RATE_TOL_PPM, "%.2f ppm (set %.1f)" % (rate, RATE_PPM))

        r = ms[0]["resp"].msg if ms else None
        check("Pdelay_Resp fields", r is not None and r.hdr.two_step and (r.hdr.log_interval & 0xFF) == 0x7F
              and r.requesting == init.src, repr(r.hdr) if r else "none")

        # drops and a twin
        resp.drop_next = 2
        lost = [init.measure(0.2) for _ in range(3)]
        check("dropped responses", lost[0] is None and lost[1] is None and lost[2] is not None,
              "%s" % ["answered" if x else "dropped" for x in lost])

        resp.twin = bytes.fromhex("020000fffe0000ee")
        since = time.clock_gettime_ns(time.CLOCK_REALTIME)
        init.measure()
        time.sleep(0.1)
        n = len([c for c in b.frames(types=gptp.PDELAY_RESP, since=since) if c.msg.hdr.seq == init.seq])
        check("twin responder", n == 2, "%d Pdelay_Resp to one request" % n)

        resp.twin = None
        resp.stop()

        # ---- a grandmaster on domain 1, received as reverse sync ----
        rx = ReverseSyncReceiver(b, 1)
        gm = GrandMaster(a, domain=1, log_sync=-3, time_offset_ns=GM_OFFSET_NS)

        time.sleep(2.0)
        gm.stop()

        te = rx.time_errors(0.0)
        e = median(te) if te else float("nan")

        check("egress time error", len(te) >= 12 and abs(e - GM_OFFSET_NS) <= TE_TOL_NS,
              "%d samples, median %.0f ns (sender ahead by %d)" % (len(te), e, GM_OFFSET_NS))

        syncs = b.frames(types=gptp.SYNC, domain=1)
        ivs = [(y.ts - x.ts) / 1e9 for x, y in zip(syncs, syncs[1:])]
        check("grandmaster Sync interval", ivs and abs(median(ivs) - 0.125) < 0.01, "median %.4f s" % median(ivs))

        anns = b.frames(types=gptp.ANNOUNCE, domain=1)
        check("grandmaster Announce", anns and anns[0].msg.priority1 == 246 and anns[0].msg.path_trace == [a.identity],
              "%d Announce, %r" % (len(anns), anns[0].msg.vector() if anns else None))

        rx.stop()

        # ---- signaling ----
        since = time.clock_gettime_ns(time.CLOCK_REALTIME)
        send_signaling(a, gptp.MsgIntervalReqTlv(1, -2, gptp.INTERVAL_STOP), seq=9)
        c = b.wait_frame(lambda c: c.type == gptp.SIGNALING, 1.0, since=since)
        check("signaling", c is not None and c.msg.interval_req == gptp.MsgIntervalReqTlv(1, -2, gptp.INTERVAL_STOP),
              repr(c.msg.interval_req) if c else "not received")

    finally:
        a.close()
        b.close()

    print("SELFTEST: " + ("PASS" if failures == 0 else "FAIL (%d)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
