# Local patches to the vendored `esp_hosted` component

This component is **vendored** (checked into the repo) and wired in via
`override_path` in [`main/idf_component.yml`](../../../main/idf_component.yml):

```yaml
espressif/esp_hosted:
  version: "2.11.0"
  override_path: "../components/espressif__esp_hosted"
```

> **The declared version is a relabel, not an upgrade.** The source here is
> still **1.4.0 + local patches** (see below). `esp_wifi_remote` 1.6.4, required
> by ESP-IDF 6.1, declares a dependency on `esp_hosted` 2.x, so the vendored
> component's `idf_component.yml` reports `2.11.0` purely to satisfy the
> dependency solver. Manifest bounds are metadata; the 1.4.x host code compiles
> and runs against `esp_wifi_remote` 1.6.4, and the host/slave `major.minor`
> lockstep that actually matters is still 1.4.x ↔ the C6's 1.4.1 firmware
> (verified at runtime: `ESP32-C6 ESP-Hosted co-processor FW version: 1.4.1`,
> WiFi associates and obtains a DHCP lease).

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
| 0003 | `CMakeLists.txt`, `host/drivers/rpc/**`, `host/port/**` | **ESP-IDF 6.x compatibility.** 1.4.0 predates IDF 6 and does not build against it. See the breakdown below. | n/a - port required by the IDF 6.1 migration |

### What patch 0003 changes, and why

| Change | Reason |
|---|---|
| `CMakeLists.txt`: add `esp_driver_sdmmc` to public `REQUIRES` | IDF 6 split the monolithic `driver` component into `esp_driver_*`. The SDMMC include is pulled in from a **public** header, so the requirement must be public too. |
| `host/port/include/os_wrapper.h`: `MEM_ALLOC` uses `heap_caps_malloc(..., MALLOC_CAP_SPIRAM \| MALLOC_CAP_CACHE_ALIGNED \| MALLOC_CAP_8BIT)` with a `MALLOC_CAP_DMA \| MALLOC_CAP_CACHE_ALIGNED` fallback | IDF 6 **removed** `esp_dma_capable_malloc()` / `esp_dma_mem_info_t`. This is the replacement Espressif documents in `migration-guides/release-6.x/6.0/peripherals.rst`. **The caps must not be combined as `MALLOC_CAP_DMA \| MALLOC_CAP_SPIRAM`:** heap caps are ANDed, and on the ESP32-P4 the DMA-capable regions are internal, so that request is unsatisfiable and *always* fell through to the internal-only fallback - silently defeating patch 0001's PSRAM preference. The PSRAM attempt must therefore ask for PSRAM alone, with internal DMA memory as the fallback. |
| `host/port/src/os_wrapper.c`: `hosted_malloc_align()` uses `heap_caps_aligned_alloc()` | Same removal as above. |
| `host/drivers/rpc/core/rpc_req.c`, `rpc_rsp.c`: stop reading/writing `wifi_sta_config_t.reserved` / `.he_reserved` | IDF 6 renamed these to `reserved1`/`reserved2` and carved real feature bits out of them. RPC serialization is **field-by-field protobuf**, not a raw struct copy, so the wire format to the C6 is unchanged: the host now sends literal `0` for the reserved bits (reserved bits must be zero) instead of forwarding bits the 1.4.1 slave would misinterpret. |
| `host/drivers/rpc/wrap/rpc_wrap.c`: add a `default:` case to the HTTP event switch | IDF 6 added `HTTP_EVENT_ON_STATUS_CODE` / `HTTP_EVENT_ON_HEADERS_COMPLETE`, which trip `-Werror=switch`. |

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
git apply -p1 ../patches/0003-idf-6.x-compat.patch

# 3. Diff the result against the vendored copy -- the ONLY expected difference
#    is the `version:` field in idf_component.yml (the 2.11.0 relabel described
#    above, which is metadata rather than a source change).
diff -ru pristine <this component dir>
```

If that diff is non-empty, the vendored source has drifted from
`upstream + patches` and either the code or these patch files need updating.
