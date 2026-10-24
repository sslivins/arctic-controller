#!/usr/bin/env bash
# Print the base URL the controller currently serves its API on.
#
#   device_url.sh [url]     (defaults to $ARCTIC_URL; only the host is used)
#
# The scheme depends on the firmware and its state, and can change across a
# reboot: HTTP mode (no administrator certificate) serves everything on port
# 80 and closes 443, HTTPS mode serves the API on 443 only, and releases from
# before HTTP mode served HTTPS with the device identity certificate. So probe
# 443 first and fall back to HTTP. When nothing answers, the HTTP URL is
# printed so that callers' retry loops keep polling.
set -uo pipefail
url="${1:-${ARCTIC_URL:?ARCTIC_URL is not set}}"
host="${url#*://}"
host="${host%%/*}"
if curl -sk --connect-timeout 2 --max-time 4 -o /dev/null "https://${host}/api/health"; then
  echo "https://${host}"
else
  echo "http://${host}"
fi
