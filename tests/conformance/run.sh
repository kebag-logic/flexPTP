#!/bin/sh

# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026

# The gPTP conformance suite: flexptpd against the Python tester across a veth
# pair, in a private user and network namespace (unshare -rn), so it needs no
# root and touches no real interface. vethA is the DUT (02:00:00:00:00:0a),
# vethB the tester (02:00:00:00:00:0b).
#
# usage: run.sh [--selftest-only] [run.py options, e.g. -k g2]
#
#   FLEXPTPD  the daemon (default: build/flexptpd at the top of the tree, as
#             "cmake -S linux -B build && cmake --build build" leaves it)
#   OUT       the evidence directory (default: a new tests/conformance/out/<time>)
#
# The library and tester self-tests run first, then every check. Exit 0 when
# all passed.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	FLEXPTPD=$(realpath -m "${FLEXPTPD:-$HERE/../../build/flexptpd}")
	OUT=$(realpath -m "${OUT:-$HERE/out/$(date +%Y%m%d-%H%M%S)}")
	mkdir -p "$OUT"
	export FLEXPTPD OUT
	exec unshare -rn sh "$0" --inside "$@"
fi
shift

ip link set lo up
ip link add vethA address 02:00:00:00:00:0a type veth peer name vethB address 02:00:00:00:00:0b
ip link set vethA up
ip link set vethB up

# veth reports carrier once both ends are up
sleep 0.3

echo "=== self-tests"
python3 "$HERE/gptp.py"
python3 "$HERE/status.py"
python3 "$HERE/selftest_tester.py" vethA vethB

if [ "$1" = --selftest-only ]; then
	exit 0
fi

echo "=== checks against $FLEXPTPD"
if [ ! -x "$FLEXPTPD" ]; then
	echo "no flexptpd at $FLEXPTPD (set FLEXPTPD)" >&2
	exit 2
fi

python3 "$HERE/run.py" --flexptpd "$FLEXPTPD" --dut-if vethA --tester-if vethB --out "$OUT" "$@"
