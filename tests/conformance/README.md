# gPTP conformance suite

`flexptpd` against a Python tester across a veth pair, in a private user and
network namespace (`unshare -rn`): no root, no real interface touched. vethA
is the DUT, vethB the tester. Both run on software timestamps; the tester's
clock is `CLOCK_REALTIME`, the DUT's a virtual clock over it (`-s -O -D`).

```sh
cmake -S linux -B build && cmake --build build      # build/flexptpd
tests/conformance/run.sh                            # self-tests, then every check
tests/conformance/run.sh -k g2                      # the checks whose name holds "g2"
tests/conformance/run.sh --selftest-only
FLEXPTPD=tests/conformance/mock_flexptpd.py tests/conformance/run.sh   # the harness against a test double
```

Exit 0 when everything passed. One `PASS`/`FAIL` line per check ID; the
evidence (a pcap and the DUT log per check, `summary.txt`) goes to
`tests/conformance/out/<time>/`, or `$OUT`.

## Files

| File | What |
|---|---|
| `gptp.py` | 802.1AS frames: build and parse every message and TLV; self-test |
| `status.py` | the `/dev/shm` status block (seqlock, `PtpGptpStatus` mirrored with ctypes and checked against `gptp_status.h`); self-test |
| `tester.py` | the L2 endpoint (software TX/RX timestamps, capture, pcap) and the roles: `GrandMaster`, `PdelayResponder`, `PdelayInitiator`, `ReverseSyncReceiver`, `send_signaling()` |
| `selftest_tester.py` | the tester against itself, no DUT |
| `dut.py` | `flexptpd` started per check, per the daemon's command-line contract |
| `checks.py` | the checks and their tolerances |
| `run.py`, `run.sh` | the runner, and the namespace around it |
| `mock_flexptpd.py` | a test double of the daemon, to exercise the harness; passing against it proves the harness, not the standard |

## Checks

| ID | Asserted | Tolerance |
|---|---|---|
| G1.1 | a 2 s link drop: link down seen, status still refreshed, SLAVE again | within 10 s |
| G1.3a/b | idle CPU with stdin at EOF; SIGTERM | <= 2 %; exit within 1 s |
| G1.4 | runs on an interface without IPv4 | |
| G2.1 | Pdelay_Req interval in LISTENING, MASTER, SLAVE | 1 s +/-10 % |
| G2.2a | neighborRateRatio = (1 + responder rate) / (1 + DUT drift), 50 ppm / 20 ppm | 1 ppm |
| G2.2b | meanLinkDelay in 0..100 us, and the tester's own measurement | within 50 us |
| G2.3a-d | asCapable drops: link delay above the threshold; the 10th lost response (not the 9th); two responders; the DUT's own identity; and comes back | drop within 4 s (2.5 s after the 10th loss), back within 6 s |
| G2.4a/b | the DUT's Pdelay_Resp: turnaround; majorSdoId 1, two-step, logMessageInterval 0x7F, requestingPortIdentity, sequenceId, domain 0, controlField 5, t3 >= t2 | 10 ms |
| G3.1 | each priority-vector field one better / one worse: SLAVE / MASTER; a path trace holding the DUT and stepsRemoved 255 are ignored | role within 6 s |
| G3.2a/b | Announce stops / Sync stops: reselection (counter or state) | 2.5..5 s / 0.25..1.2 s |
| G3.3 | Sync/Follow_Up 5 ms ahead from a foreign port identity: clock unmoved, syncRx 0; the same from the announcing port is used | 1 ms |
| G3.4a-e | as grandmaster: Announce fields (248/248, GM identity byte order, stepsRemoved 0, path trace [DUT], ptpTimescale); Announce 1 s; Sync two-step -3 with paired Follow_Up + TLV; Sync 125 ms; nothing sent once not asCapable | +/-10 % |
| G4.1 | message interval requests 1/-2/1, then -128, 126, 127 (pdelay) | +/-10 % |
| G4.2 | a foreign-organization and a short TLV change nothing | |
| G5.1 | reverse sync off by default; on: domain 1 only, two-step, Follow_Up TLV, nothing on domain 0; default 90 % of the Sync interval, `interval 200`, `rate 50` | +/-5 % |
| G5.2 | time error by the egress method = the DUT's reported timeError = its virtual clock error | 50 us |
| G5.3 | the grandmaster stops: reverse sync goes on | >= 80 % of the expected count |

On veth with software timestamps the link delay measures 1-3 us, above
802.1AS's 800 ns, so every check runs with `ptp gptp nbthresh 100000`.
`ptp rsync rate PCT` is taken as PCT of the incoming Sync *interval*
(`RSYNC_RATE_IS_INTERVAL_PCT` in `checks.py`).
