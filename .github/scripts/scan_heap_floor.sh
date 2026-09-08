#!/usr/bin/env bash
#
# Canonical internal-heap floor scanner for the device-tests serial log.
#
# This is the SINGLE SOURCE OF TRUTH for the heap-headroom gate, shared by:
#   - .github/workflows/device-tests.yml  ("Check internal heap headroom")
#   - tests/api/test_heap_floor.py        (hostside regression test)
# so the two can never drift.
#
# WHY THIS GATE EXISTS
#
# Internal (non-PSRAM) RAM is the controller's scarcest resource. Steady-state
# free internal heap is only ~30-40 KB while PSRAM sits at ~23 MB, so any log
# line reporting a generic "free heap" in the megabytes says nothing about
# health: a 176-byte allocation can still fail because the caller needed
# internal memory specifically (lwIP PCBs, TLS state, DMA, task stacks).
#
# When internal RAM does run out, the device does not crash and does not report
# a device fault. It stops completing TCP handshakes. To the test suite that
# appears as ConnectTimeout, ConnectionError, ChunkedEncodingError and websocket
# TimeoutError scattered across unrelated suites -- indistinguishable from
# runner flake, and in fact dismissed as flake more than once before issue #234.
# The gate exists to attribute those symptoms to their real cause.
#
# WHICH NUMBER TO GATE ON (this is the subtle part)
#
# netdiag prints two figures: instantaneous free internal heap ("heap int=N")
# and the since-boot low-water mark ("(min M)", heap_caps_get_minimum_free_size).
#
# The low-water M is NOT usable as a gate. It is monotonic and never resets, so
# a single brief early transient pins it near zero for the rest of the boot. In
# practice it reads 16 bytes on EVERY observed run -- passing and failing alike
# (7 of 7 sampled) -- which makes it useless for telling good runs from bad.
# It is reported below as context only.
#
# The instantaneous figure separates cleanly. Across 7 sampled runs:
#     5 healthy runs   min instantaneous free = 24847 .. 32727 bytes
#     2 wedged runs    min instantaneous free = 23 bytes
# and both wedged runs also logged "PCB walk TIMED OUT" (the lwIP tcpip thread
# failing to service a callback within 1s) -- the same event seen from the other
# side. One of those two wedged runs failed five tests; the other survived but
# was equally sick. So this gate fires on the condition, not on whether the
# tests happened to get away with it.
#
# Usage:   scan_heap_floor.sh <serial-log> [floor-bytes]
# Output:  a one-line summary on stdout, plus offending samples on violation.
# Exit:    0  healthy (instantaneous free stayed >= floor, no PCB-walk timeout)
#          1  violation (dipped below floor, or the tcpip thread timed out)
#          2  unusable input (missing log, or no netdiag samples at all)
set -uo pipefail

log="${1:?usage: scan_heap_floor.sh <serial-log> [floor-bytes]}"
floor="${2:-8192}"

if [ ! -f "$log" ]; then
    echo "scan_heap_floor: serial log '$log' not found" >&2
    exit 2
fi

# Matches both the healthy line and the "PCB walk TIMED OUT" variant, since both
# carry the heap figures:
#   I (33395) netdiag: heap int=40615 (min 12632) psram=23673156 | tcp active=1 ...
NETDIAG_RE='netdiag: heap int=[0-9]+ \(min [0-9]+\)'

samples="$(grep -cE "$NETDIAG_RE" "$log" || true)"
if [ "${samples:-0}" -eq 0 ]; then
    # Fail closed. A run with no netdiag samples has not demonstrated health,
    # it has only failed to observe. Passing here would recreate the silent
    # "no data == healthy" bug that once let real crashes ship.
    echo "::error title=No netdiag samples::'$log' contains no netdiag lines, so internal-heap headroom was never observed. The gate cannot pass on absent data." >&2
    exit 2
fi

# The gated metric: lowest instantaneous free internal heap across the run.
min_instant="$(grep -oE 'netdiag: heap int=[0-9]+' "$log" \
    | grep -oE '[0-9]+$' \
    | sort -n | head -1)"

# Context only -- saturates at ~16 bytes on every run, see the note above.
lowwater="$(grep -oE 'heap int=[0-9]+ \(min [0-9]+\)' "$log" \
    | grep -oE '\(min [0-9]+\)' \
    | grep -oE '[0-9]+' \
    | sort -n | head -1)"

# Peak TIME_WAIT population: internal-RAM-backed PCBs accumulating under churn,
# governed by CONFIG_LWIP_TCP_MSL. Context for why headroom evaporates.
maxtw="$(grep -oE 'tw=[0-9]+' "$log" | grep -oE '[0-9]+' | sort -n | tail -1)"
maxtw="${maxtw:-0}"

timeouts="$(grep -cE 'netdiag: .*PCB walk TIMED OUT' "$log" || true)"
timeouts="${timeouts:-0}"

echo "netdiag samples=${samples} min-instantaneous-internal-heap=${min_instant} bytes (floor=${floor}) since-boot-low-water=${lowwater} peak-TIME_WAIT=${maxtw} pcb-walk-timeouts=${timeouts}"

status=0

if [ "${min_instant:-0}" -lt "$floor" ]; then
    echo "::error title=Internal heap exhausted::Instantaneous free internal (non-PSRAM) heap fell to ${min_instant} bytes, below the ${floor}-byte floor. A healthy run on this hardware does not drop below ~24 KB, so this is a genuine wedge: in this state the device stops completing TCP handshakes. Any ConnectTimeout / ConnectionError / premature-response / websocket-timeout failures in this run are almost certainly caused by this and are NOT runner flake. See issue #234. Peak TIME_WAIT was ${maxtw}. Lowest samples:" >&2
    grep -nE "$NETDIAG_RE" "$log" \
        | awk 'match($0, /heap int=[0-9]+/) {
                   v = substr($0, RSTART + 9, RLENGTH - 9) + 0
                   printf "%d\t%s\n", v, $0
               }' \
        | sort -n | head -10 | cut -f2- >&2
    status=1
fi

if [ "$timeouts" -gt 0 ]; then
    echo "::error title=tcpip thread unresponsive::${timeouts} netdiag sample(s) reported 'PCB walk TIMED OUT', meaning the lwIP tcpip thread did not service a callback within 1s. On every run observed so far this coincides with internal-heap exhaustion; it is the same wedge seen from the network-stack side. Treat it as a device fault, not a flaky runner." >&2
    grep -nE 'netdiag: .*PCB walk TIMED OUT' "$log" | head -5 >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "OK: instantaneous free internal heap stayed at or above the ${floor}-byte floor (lowest ${min_instant})."
fi

exit "$status"
