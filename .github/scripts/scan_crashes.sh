#!/usr/bin/env bash
#
# Canonical firmware-crash signature scanner for the device-tests serial log.
#
# This is the SINGLE SOURCE OF TRUTH for the crash regex, shared by:
#   - .github/workflows/device-tests.yml  ("Detect firmware crashes on device")
#   - tests/api/test_crash_detection.py   (hostside regression test)
# so the two can never drift.
#
# Only two signals are trustworthy crash evidence, and neither can be produced
# by a test. Device/API tests deliberately SEED event-log entries named
# "application_crash", "watchdog_reset" and "brownout_reset" to exercise the
# event-log UI; those are journalled events, NOT crashes, and must be ignored:
#   - "Crash reboot ("  : printed by boot_stats on the next boot ONLY when the
#                         hardware reset reason is PANIC or a watchdog
#                         (main/boot_stats.cpp is_crash_reason), so it is
#                         derived from the hardware reset reason and can never
#                         be produced by a seeded event.
#   - "Guru Meditation" : a live panic dump printed on the serial line.
#
# Usage:   scan_crashes.sh <serial-log>
# Output:  matching "<lineno>:<line>" records on stdout (grep -nE format).
# Exit:    0 if at least one crash signature is found,
#          1 if none are found (or the log file is missing).
set -uo pipefail

# Keep this regex in sync ONLY here. tests/api/test_crash_detection.py pins the
# match/no-match behaviour against representative fixture lines.
#
# Signatures, and why each is safe (no test legitimately emits them):
#   Guru Meditation          - exception panic dump header (covers Load/Store/
#                              Illegal-instruction causes shown on that line).
#   Crash reboot \(          - boot_stats journalling the PREVIOUS boot's crash.
#   A stack overflow in task - FreeRTOS stack-overflow hook. NOTE: this path
#                              prints NO "Guru Meditation" line, so without this
#                              signature an overflow is only caught on the *next*
#                              boot's "Crash reboot (" - and missed entirely if
#                              the run ends first (this actually happened).
#   abort() was called       - abort()/assert-driven panic.
#   assert failed:           - ESP-IDF assertion panic.
#   CORRUPT HEAP             - heap-corruption detector panic.
CRASH_RE='Guru Meditation|Crash reboot \(|A stack overflow in task|abort\(\) was called|assert failed:|CORRUPT HEAP'

log="${1:?usage: scan_crashes.sh <serial-log>}"
[ -f "$log" ] || exit 1

hits="$(grep -nE "$CRASH_RE" "$log" || true)"
[ -n "$hits" ] || exit 1

printf '%s\n' "$hits"
exit 0
