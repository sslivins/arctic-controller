#!/usr/bin/env bash
# Build an NVS partition image that lets the bench controller join Wi-Fi and
# accept a known API key on first boot (auth_mgr_init() uses a pre-seeded
# "auth/api_key" instead of generating a random one).
#
# Usage: gen_bench_nvs.sh <output.bin>
# Requires WIFI_SSID, WIFI_PASSWORD and ARCTIC_API_KEY in the environment.
set -euo pipefail

out="$1"
csv=$(mktemp)
trap 'rm -f "$csv"' EXIT

{
  echo "key,type,encoding,value"
  echo "wifi_creds,namespace,,"
  echo "ssid,data,string,${WIFI_SSID}"
  echo "password,data,string,${WIFI_PASSWORD}"
  echo "auth,namespace,,"
  echo "api_key,data,string,${ARCTIC_API_KEY}"
} > "$csv"

python3 -m esp_idf_nvs_partition_gen generate "$csv" "$out" 0x40000
