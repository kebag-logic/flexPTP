#!/bin/sh

# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026

# The flexptpd smoke test: two flexptpd instances in software-timestamp mode
# on the two ends of a veth pair, in a private user and network namespace
# (unshare -rn: no root needed, no real interface touched).
#
#   veth0  master  priority1 100, virtual clock = CLOCK_REALTIME
#   veth1  slave   priority1 248, virtual clock 50 ms ahead, drifting 20 ppm
#
# Checks:
#   S.1   the slave reaches SLAVE and the master MASTER (status blocks)
#   S.2   the slave's virtual clock converges: |vclock - CLOCK_REALTIME| <=
#         100 us over its last 5 one-second reports (the master runs on
#         CLOCK_REALTIME itself)
#   G1.4  neither interface has an IPv4 address
#   G1.3  stdin at end of file, no terminal: under 2 % CPU over 10 s
#   G1.1  veth1 down for 2 s: the slave keeps running, its status block says
#         linkUp 0, it settles back in SLAVE (SLAVE held for 3 s) within 10 s
#         of link-up, and its clock is then within 100 us (its sends fail:
#         ENETDOWN)
#   G1.1b the peer end (veth0) down for 3 s: veth1 loses its carrier, and the
#         kernel drops the slave's frames silently (sendto() succeeds, no
#         timestamp ever comes). Starting from a settled SLAVE, the
#         timestamps are reported lost by the driver's timeout, and the slave
#         keeps running and settles back in SLAVE within 10 s of link-up
#   G1.3  SIGTERM: both gone within 1 s, exit status 0
#   G1.3  -P: the threads run SCHED_FIFO (only where permitted, else SKIP)
#
# usage: run-netns-smoke.sh [BUILD_DIR]     (default: build/ at the top of the tree)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)

if [ "${1:-}" != --inside ]; then
	BUILD=$(realpath "${1:-$HERE/../../build}")

	for b in flexptpd flexptpd-status; do
		if [ ! -x "$BUILD/$b" ]; then
			echo "run-netns-smoke: no $BUILD/$b (cmake -S linux -B build && cmake --build build)" >&2
			exit 2
		fi
	done

	exec unshare -rn sh "$0" --inside "$BUILD"
fi

BUILD=$2
DAEMON=$BUILD/flexptpd
STATUS=$BUILD/flexptpd-status

TMP=$(mktemp -d)
TAG=smoke-$$
MPID=
SPID=

FAILED=0

pass() {
	echo "[PASS] $*"
}

fail() {
	echo "[FAIL] $*"
	FAILED=$((FAILED + 1))
}

cleanup() {
	for p in $MPID $SPID; do
		kill -9 "$p" 2>/dev/null
	done

	rm -f "/dev/shm/$TAG-m" "/dev/shm/$TAG-s" "/dev/shm/$TAG-p"
	rm -rf "$TMP"
}
trap cleanup EXIT

# a field of a status block: field NAME KEY
field() {
	"$STATUS" "$1" 2>/dev/null | awk -v k="$2" '$1 == k { print $2 }'
}

# wait until a field has a value: wait_field NAME KEY VALUE SECONDS
wait_field() {
	i=0
	while [ "$i" -lt $(($4 * 10)) ]; do
		if [ "$(field "$1" "$2")" = "$3" ]; then
			return 0
		fi

		sleep 0.1
		i=$((i + 1))
	done

	return 1
}

# the largest |vclock error| of the last N reports in a log: last_error LOG N
last_error() {
	grep '^vclock-error-ns' "$1" | tail -n "$2" | awk '
		{ v = $2 < 0 ? -$2 : $2; if (v > m) m = v; n++ }
		END { if (n == 0) print "none"; else print m }'
}

# Wait for the slave to settle in SLAVE (held for 3 s), at most LIMIT seconds
# after T0 (ns); print the time of its last entry into SLAVE in ms after T0,
# or "none": settle NAME T0_NS LIMIT
settle() {
	since=
	end=$(($2 + $3 * 1000000000))

	while [ "$(date +%s%N)" -lt "$end" ]; do
		now=$(date +%s%N)

		if [ "$(field "$1" portState)" = SLAVE ]; then
			if [ -z "$since" ]; then
				since=$now
			fi

			if [ $((now - since)) -ge 3000000000 ]; then
				echo $(((since - $2) / 1000000))
				return 0
			fi
		else
			since=
		fi

		sleep 0.1
	done

	echo none
	return 1
}

# CPU time of a process in clock ticks
cpu_ticks() {
	awk '{ print $14 + $15 }' "/proc/$1/stat"
}

# ---- the link ----

ip link set lo up
ip link add veth0 type veth peer name veth1
ip link set veth0 up
ip link set veth1 up
sleep 0.2

printf 'ptp priority 100 100\n' > "$TMP/master.cfg"

# ---- start ----

"$DAEMON" -i veth0 -s -c "$TMP/master.cfg" -m "$TAG-m" -q </dev/null >"$TMP/master.log" 2>&1 &
MPID=$!

"$DAEMON" -i veth1 -s -O 50000000 -D 20000 -m "$TAG-s" -v </dev/null >"$TMP/slave.log" 2>&1 &
SPID=$!

# ---- S.1 ----

if wait_field "$TAG-s" portState SLAVE 30; then
	pass "S.1 the slave reaches SLAVE"
else
	fail "S.1 the slave reaches SLAVE (portState $(field "$TAG-s" portState))"
fi

if [ "$(field "$TAG-m" portState)" = MASTER ]; then
	pass "S.1 the master is MASTER"
else
	fail "S.1 the master is MASTER (portState $(field "$TAG-m" portState))"
fi

# ---- S.2 ----

converged=no
i=0
while [ "$i" -lt 40 ]; do
	e=$(last_error "$TMP/slave.log" 5)

	if [ "$e" != none ] && [ "$(grep -c '^vclock-error-ns' "$TMP/slave.log")" -ge 5 ] && [ "$e" -le 100000 ]; then
		converged=yes
		break
	fi

	sleep 1
	i=$((i + 1))
done

if [ "$converged" = yes ]; then
	pass "S.2 the slave's clock converges: worst |error| of the last 5 s $e ns (<= 100000)"
else
	fail "S.2 the slave's clock converges: worst |error| of the last 5 s $(last_error "$TMP/slave.log" 5) ns"
fi

# ---- G1.4 ----

if ! ip -4 addr show dev veth0 | grep -q inet && ! ip -4 addr show dev veth1 | grep -q inet; then
	pass "G1.4 both run without an IPv4 address"
else
	fail "G1.4 an interface has an IPv4 address"
fi

# ---- G1.3: idle CPU with stdin at end of file ----

hz=$(getconf CLK_TCK)
t0=$(cpu_ticks "$SPID")
sleep 10
t1=$(cpu_ticks "$SPID")

pct=$(awk -v d=$((t1 - t0)) -v hz="$hz" 'BEGIN { printf "%.2f", 100 * d / (10 * hz) }')

if awk -v p="$pct" 'BEGIN { exit !(p < 2) }'; then
	pass "G1.3 stdin at end of file: $pct % CPU over 10 s (< 2 %)"
else
	fail "G1.3 stdin at end of file: $pct % CPU over 10 s"
fi

# ---- G1.1: the link drops for 2 s ----

lost0=$(field "$TAG-s" txTimestampsLost)

ip link set veth1 down

if wait_field "$TAG-s" linkUp 0 2; then
	pass "G1.1 the status block reports the link down"
else
	fail "G1.1 the status block reports the link down (linkUp $(field "$TAG-s" linkUp))"
fi

sleep 2
during=$(field "$TAG-s" portState)

ip link set veth1 up

if kill -0 "$SPID" 2>/dev/null; then
	pass "G1.1 the slave keeps running through the drop (portState $during at its end)"
else
	fail "G1.1 the slave keeps running through the drop"
fi

up_at=$(date +%s%N)

back_ms=$(settle "$TAG-s" "$up_at" 15)

if [ "$back_ms" != none ] && [ "$back_ms" -le 10000 ]; then
	pass "G1.1 settled back in SLAVE $back_ms ms after link-up (<= 10 s)"
else
	fail "G1.1 settled back in SLAVE within 10 s of link-up (at $back_ms ms; portState $(field "$TAG-s" portState))"
fi

# synchronization goes on: the error is bounded once settled
e=$(last_error "$TMP/slave.log" 3)
lost1=$(field "$TAG-s" txTimestampsLost)

if [ "$e" != none ] && [ "$e" -le 100000 ]; then
	pass "G1.1 synchronized once settled: worst |error| of the last 3 s $e ns ($((lost1 - lost0)) transmit timestamps lost in the drop)"
else
	fail "G1.1 synchronized once settled: worst |error| of the last 3 s $e ns"
fi

# ---- G1.1b: the carrier drops, the frames vanish ----

# from a settled SLAVE, which sends (Pdelay_Req) during the drop
settle "$TAG-s" "$(date +%s%N)" 20 >/dev/null

lost0=$(field "$TAG-s" txTimestampsLost)

ip link set veth0 down

if wait_field "$TAG-s" linkUp 0 2; then
	pass "G1.1b the slave reports its link down when the carrier goes"
else
	fail "G1.1b the slave reports its link down when the carrier goes (linkUp $(field "$TAG-s" linkUp))"
fi

sleep 3

ip link set veth0 up
up_at=$(date +%s%N)

lost1=$(field "$TAG-s" txTimestampsLost)

if kill -0 "$SPID" 2>/dev/null && [ "$((lost1 - lost0))" -ge 1 ]; then
	pass "G1.1b the slave keeps running; $((lost1 - lost0)) timestamps reported lost by the timeout"
else
	fail "G1.1b the slave keeps running, with timestamps reported lost (lost $((lost1 - lost0)))"
fi

back_ms=$(settle "$TAG-s" "$up_at" 15)

if [ "$back_ms" != none ] && [ "$back_ms" -le 10000 ]; then
	pass "G1.1b settled back in SLAVE $back_ms ms after link-up (<= 10 s)"
else
	fail "G1.1b settled back in SLAVE within 10 s of link-up (at $back_ms ms; portState $(field "$TAG-s" portState))"
fi

# ---- G1.3: SIGTERM ----

start=$(date +%s%N)
kill -TERM "$MPID" "$SPID"

i=0
while [ "$i" -lt 20 ] && { kill -0 "$MPID" 2>/dev/null || kill -0 "$SPID" 2>/dev/null; }; do
	sleep 0.05
	i=$((i + 1))
done

took=$((($(date +%s%N) - start) / 1000000))

wait "$MPID"
mrc=$?
wait "$SPID"
src=$?
MPID=
SPID=

if [ "$took" -le 1000 ] && [ "$mrc" -eq 0 ] && [ "$src" -eq 0 ]; then
	pass "G1.3 SIGTERM: both gone in $took ms, exit status 0"
else
	fail "G1.3 SIGTERM: gone in $took ms, exit status $mrc and $src"
fi

if [ -e "/dev/shm/$TAG-s" ]; then
	fail "G1.3 the status block is removed on exit"
else
	pass "G1.3 the status block is removed on exit"
fi

# ---- G1.3: SCHED_FIFO ----

if chrt -f 10 true 2>/dev/null; then
	"$DAEMON" -i veth0 -s -P 10 -m "$TAG-p" -q </dev/null >"$TMP/prio.log" 2>&1 &
	PPID_=$!
	sleep 2

	policies=$(for t in /proc/"$PPID_"/task/*; do chrt -p "${t##*/}" | awk -F': ' '/policy/ { print $2 }'; done | sort -u | tr '\n' ' ')

	kill -TERM "$PPID_"
	wait "$PPID_"

	case "$policies" in
	"SCHED_FIFO ")
		pass "G1.3 -P 10: every thread runs SCHED_FIFO"
		;;
	*)
		fail "G1.3 -P 10: thread policies $policies"
		;;
	esac
else
	echo "[SKIP] G1.3 -P: SCHED_FIFO is not permitted here (needs CAP_SYS_NICE or an RLIMIT_RTPRIO)"
fi

# ---- verdict ----

if [ "$FAILED" -ne 0 ]; then
	echo "--- slave log (tail) ---"
	tail -n 30 "$TMP/slave.log"
	echo "--- master log (tail) ---"
	tail -n 15 "$TMP/master.log"
fi

if [ "$FAILED" -eq 0 ]; then
	echo "netns smoke: PASS"
	exit 0
fi

echo "netns smoke: FAIL ($FAILED failed)"
exit 1
