#!/usr/bin/env bash
# Download the core dump the controller saved on its last crash.
#
#   fetch_coredump.sh <output-file>
#
# Needs ARCTIC_URL (https://...) and ARCTIC_API_KEY. Exit codes:
#   0  a dump was saved to <output-file>
#   1  the device has no dump
#   2  the device could not be asked (unreachable, auth, bad response)
set -uo pipefail

out="${1:?usage: fetch_coredump.sh <output-file>}"
url="${ARCTIC_URL/http:/https:}/api/logs/coredump"

for attempt in 1 2 3 4 5 6; do
  code=$(curl -sk --connect-timeout 5 --max-time 60 -o "$out" -w '%{http_code}' \
    -H "X-API-Key: ${ARCTIC_API_KEY}" "$url" 2>/dev/null) || code=000
  case "$code" in
    200)
      # The raw dump starts with its own length (little-endian uint32); a
      # mismatch means the transfer was cut short.
      want=$(od -An -tu4 -N4 "$out" | tr -d ' ')
      have=$(stat -c %s "$out")
      if [ "$want" = "$have" ]; then
        echo "Core dump saved to $out ($have bytes)."
        exit 0
      fi
      echo "Core dump transfer incomplete ($have of $want bytes), retrying..."
      ;;
    404)
      rm -f "$out"
      echo "No core dump on the device."
      exit 1
      ;;
    *)
      echo "GET /api/logs/coredump returned HTTP $code, retrying..."
      ;;
  esac
  sleep 5
done
rm -f "$out"
echo "::warning::Could not ask the controller for a core dump."
exit 2
