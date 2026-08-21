# Local patches to the vendored `esp_hosted` component

This component is **vendored** (checked into the repo) and wired in via
`override_path` in [`main/idf_component.yml`](../../../main/idf_component.yml):

```yaml
espressif/esp_hosted:
  version: "1.4.0"
  override_path: "../components/espressif__esp_hosted"
```

It is **not** pristine upstream — it carries the local modifications listed
below. The patches are applied **in place** (edited directly in the checked-in
source); the `.patch` files under [`patches/`](patches/) are kept as
**reference/documentation only** and are **not** applied by the build.

## Base version

| | |
|---|---|
| Upstream component | `espressif/esp_hosted` |
| Base version | **1.4.0** (Espressif component registry) |
| Base zip | https://components-file.espressif.com/components/espressif/esp_hosted/1.4.0/espressif__esp_hosted-v1.4.0.zip |

## Why we patch instead of upgrading

The Tab5's ESP32-C6 radio runs ESP-Hosted **1.4.1** (confirmed at runtime via
`esp_hosted_get_coprocessor_fwversion()`, surfaced as `wifi_module_fw_version`
in `GET /api/info`). ESP-Hosted requires the host and slave to match on
**major.minor**, so the host driver is locked to the **1.4.x** line until the
C6 is bench-reflashed. Both fixes below exist upstream only in **2.x+**, so they
must be **backported** into 1.4.0 rather than picked up via an upgrade.

## The patches

| # | File | Purpose | Upstream origin |
|---|------|---------|-----------------|
| 0001 | `host/port/include/os_wrapper.h` | `MEM_ALLOC` prefers PSRAM (`MALLOC_CAP_SPIRAM`) with fallback to internal DMA SRAM, so the streaming SDIO RX buffer doesn't exhaust internal DMA SRAM during an OTA download (fixes `sdio_rx_get_buffer:670` OOM assert). | esp-hosted-mcu 2.12.8 `MEMPOOL_PREFER_SPIRAM` |
| 0002 | `host/drivers/virtual_serial_if/serial_if.c` | `transport_pserial_send` no longer double-frees `write_buf` after `serial_drv_write()` consumes it (ownership passes to `esp_hosted_tx()`, which frees it on the `!transport_up` branch). Fixes `tlsf_free` "block already marked as free" panic when the C6 link is down. | esp-hosted-mcu `main` `free_bufs1`/`free_bufs2` split |

## Removal criteria (important)

**Both patches are a stopgap for the 1.4.x lockstep.** ESP-Hosted **2.12.12**
contains *both* fixes natively (the double-free fix and `MEMPOOL_PREFER_SPIRAM`).
Once the C6 is reflashed to 2.12.x (see the
[`arctic-c6-slave`](https://github.com/sslivins/arctic-c6-slave) build repo) and
the host is bumped to a matching 2.12.x, the correct action is:

1. Delete this vendored component directory.
2. Remove the `override_path` from `main/idf_component.yml` and bump
   `espressif/esp_hosted` to the matching registry version.
3. Delete this file and `patches/`.

i.e. the goal is to carry **zero** local patches, not to maintain this fork.

## Verifying / re-applying the patches

To confirm the vendored tree still equals `upstream 1.4.0 + these patches`
(useful when reviewing, or to re-derive after a version bump):

```bash
# 1. Fetch pristine 1.4.0 into a scratch dir
curl -L -o esp_hosted-1.4.0.zip \
  https://components-file.espressif.com/components/espressif/esp_hosted/1.4.0/espressif__esp_hosted-v1.4.0.zip
unzip esp_hosted-1.4.0.zip -d pristine

# 2. Apply the reference patches from the component root
cd pristine
git apply -p1 ../patches/0001-mempool-prefer-spiram.patch
git apply -p1 ../patches/0002-serial_if-double-free.patch

# 3. Diff the result against the vendored copy -- expect NO differences
diff -ru pristine <this component dir>
```

If that diff is non-empty, the vendored source has drifted from
`upstream + patches` and either the code or these patch files need updating.
