# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""run.py - run the conformance checks against flexptpd. Inside the network
namespace run.sh builds; on its own only with an existing veth pair.

usage: run.py --flexptpd BIN --dut-if IF --tester-if IF --out DIR [-k PATTERN] [--list]

Prints one PASS / FAIL line per check ID, writes DIR/summary.txt, and one
pcap and DUT log per check. Exit 0 when every check passed.
"""

import argparse
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from checks import CHECKS, Context      # noqa: E402
from dut import DutError                # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--flexptpd", required=True)
    ap.add_argument("--dut-if", default="vethA")
    ap.add_argument("--tester-if", default="vethB")
    ap.add_argument("--out", required=True)
    ap.add_argument("-k", "--filter", default="", help="run the checks whose name contains this")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    selected = [(n, f) for n, f in CHECKS if args.filter in n]

    if args.list:
        for n, f in selected:
            print("%-20s %s" % (n, (f.__doc__ or "").strip().splitlines()[0]))
        return 0

    os.makedirs(args.out, exist_ok=True)

    ctx = Context(os.path.abspath(args.flexptpd), args.dut_if, args.tester_if, args.out)
    results = []

    for name, fn in selected:
        print("--- %s: %s" % (name, (fn.__doc__ or "").strip().splitlines()[0]), flush=True)
        t0 = time.monotonic()

        try:
            ctx.begin(name)
            rows = fn(ctx)
        except DutError as e:
            rows = [(name, False, "DUT: %s" % e)]
        except Exception:
            rows = [(name, False, "ERROR: " + traceback.format_exc().strip().replace("\n", " | "))]
        finally:
            ctx.end()

        for cid, ok, detail in rows:
            print("%s %-22s %s" % ("PASS" if ok else "FAIL", cid, detail), flush=True)
            results.append((cid, ok, detail))

        print("    (%.0f s)" % (time.monotonic() - t0), flush=True)

    failed = [r for r in results if not r[1]]

    with open(os.path.join(args.out, "summary.txt"), "w") as f:
        for cid, ok, detail in results:
            f.write("%s %s %s\n" % ("PASS" if ok else "FAIL", cid, detail))

    print("RESULT: %s (%d checks, %d failed), evidence in %s" % (
        "PASS" if not failed else "FAIL", len(results), len(failed), args.out))

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
