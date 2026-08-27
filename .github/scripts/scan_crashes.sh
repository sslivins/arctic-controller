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
CRASH_RE='Guru Meditation|Crash reboot \('

log="${1:?usage: scan_crashes.sh <serial-log>}"
[ -f "$log" ] || exit 1

hits="$(grep -nE "$CRASH_RE" "$log" || true)"
[ -n "$hits" ] || exit 1

printf '%s\n' "$hits"
exit 0
