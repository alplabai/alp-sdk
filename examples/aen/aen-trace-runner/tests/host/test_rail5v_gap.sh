#!/bin/bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# tests/host/test_rail5v_gap.sh -- source interlock for platform/rail5v_power.c's poll (a Zephyr file:
# no host build), the power graph's contract with the I2C2 lease (bus2_he.h):
#   1. the lease check comes BEFORE any bus transfer: the HE never touches I2C2 while the HP holds it;
#   2. a slot the poll cannot sample -- the HP holds the bus, CONFIG found reverted, a read missed,
#      the rail below 4.5 V -- pushes a GAP (never a zero, never a stale value) before it returns;
#   3. the poll never blocks (no sleep / busy-wait / wait on the lease);
#   4. a real sample is pushed raw (the graph), not the EMA.
set -u
cd "$(dirname "$0")/../.." || exit 1
f=src/platform/rail5v_power.c
fail=0
FAILS() { echo "FAIL $*"; fail=1; }

body=$(awk '/^void tr_rail5v_poll\(void\)/{p=1} p{print} p&&/^}/{exit}' "$f")
[ -n "$body" ] || { echo "FAIL cannot find tr_rail5v_poll in $f"; exit 1; }

# 1. the lease check precedes every transfer
lease=$(grep -n 'tr_bus2_he_owns()' <<<"$body" | head -1 | cut -d: -f1)
xfer=$(grep -n 'config_check()\|reg_read16(\|config_write()' <<<"$body" | head -1 | cut -d: -f1)
{ [ -n "$lease" ] && [ -n "$xfer" ] && [ "$lease" -lt "$xfer" ]; } || FAILS "the lease check (line ${lease:-none}) must precede the first bus transfer (line ${xfer:-none})"

# 2. every return after the period gate leaves a gap (or follows a pushed sample)
after=$(awk '/g_next_ms = now/{p=1;next} p{print}' <<<"$body")
prev=""
n=0
while IFS= read -r line; do
	if [[ $line =~ ^[[:space:]]*return\; ]]; then
		n=$((n + 1))
		[[ $prev == *"tr_pwr_ring_push(&g_ring, TR_PWR_GAP);"* || $prev == *"g_have_sample"* ]] ||
			FAILS "a return without a gap pushed before it: after '${prev}'"
	fi
	[ -n "${line//[[:space:]]/}" ] && prev=$line
done <<<"$after"
[ "$n" -ge 5 ] || FAILS "expected >= 5 returns after the period gate (lease, CONFIG, read, 4.5 V, seed), found $n"

# 3. never blocks
grep -qE 'k_sleep|k_msleep|k_usleep|k_busy_wait|K_FOREVER|k_sem_take|k_mutex_lock' <<<"$body" &&
	FAILS "tr_rail5v_poll must not block"

# 4. the sample goes into the graph raw
tr -d ' 	
' <<<"$body" | grep -q 'tr_pwr_ring_push(&g_ring,sample_mw)' || FAILS "the raw sample must be pushed into the ring"

if [ $fail -eq 0 ]; then echo "PASS: test_rail5v_gap.sh"; else exit 1; fi
