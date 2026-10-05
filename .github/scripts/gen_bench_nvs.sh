#!/usr/bin/env bash
# Build an NVS partition image that lets the bench controller join Wi-Fi and
# accept a known API key on first boot (auth_mgr_init() uses a pre-seeded
# "auth/api_key" instead of generating a random one), with non-factory admin
# credentials so the API is not locked behind a credential change.
#
# Usage: gen_bench_nvs.sh <output.bin>
# Requires WIFI_SSID, WIFI_PASSWORD and ARCTIC_API_KEY in the environment.
set -euo pipefail

out="$1"
csv=$(mktemp)
trap 'rm -f "$csv"' EXIT

# With no password in NVS the firmware installs the factory credentials, and
# while those are in place it rejects every API request, even with a valid key
# (check_api_auth -> auth_mgr_credentials_change_required). Seed a random
# admin password (the firmware stores an unsalted SHA-256 of it) so a freshly
# erased device is usable. Nothing signs in with it; tests use the API key.
admin_password="${ARCTIC_ADMIN_PASSWORD:-$(openssl rand -hex 16)}"
pass_hash=$(printf '%s' "$admin_password" | sha256sum | cut -d' ' -f1)

{
  echo "key,type,encoding,value"
  echo "wifi_creds,namespace,,"
  echo "ssid,data,string,${WIFI_SSID}"
  echo "password,data,string,${WIFI_PASSWORD}"
  echo "auth,namespace,,"
  echo "api_key,data,string,${ARCTIC_API_KEY}"
  echo "username,data,string,arctic"
  echo "pass_hash,data,hex2bin,${pass_hash}"
} > "$csv"

python3 -m esp_idf_nvs_partition_gen generate "$csv" "$out" 0x40000
