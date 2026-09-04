# ESP-IDF patches

Patches applied to the vendored ESP-IDF toolchain (`v5.5.2`) inside the
`espressif/esp-idf-ci-action` Docker container before building. Applied via
the action's `command:` override — see `.github/workflows/build.yml`,
`create-release.yml`, and `device-tests.yml` for the exact invocation.

## esp-idf-5.5.2-read_otadata-race-fix.patch

Fixes a race condition in `components/app_update/esp_ota_ops.c`'s
`read_otadata()` that causes a fatal, unrecoverable
`HP_SYS_HP_WDT_RESET` crash on ESP32-P4 (frozen core, `PC=0x4FF00000`).

**Root cause:** `read_otadata()` reads the `otadata` partition via
`esp_partition_mmap()` + `memcpy()`. Under our build config
(`CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` + `CONFIG_SPIRAM_RODATA=y`, i.e.
XIP-from-PSRAM), ESP-IDF's `spi_flash_os_func_app.c` sets
`SPI_FLASH_CACHE_NO_DISABLE`, which replaces the legacy cross-core
cache-disable/freeze-other-CPU guard with a plain mutex
(`s_spi1_flash_mutex`) around physical flash operations. `esp_partition_mmap()`
+ `memcpy()` never takes that mutex, so a concurrent flash erase/write on the
other core (in our case, `log_persist_task`'s debug-log flush, triggered by a
WARN-level log) can be read mid-operation — corrupted/torn data, or in the
worst case a fault while the flash chip is mid-erase and the mmap'd region is
being fetched as an instruction/data access, producing the WDT-frozen crash.

Confirmed via live JTAG on real hardware, and via ESP-IDF source inspection
(`spi_flash_os_func_app.c`'s `SPI_FLASH_CACHE_NO_DISABLE` macro). Validated by
replacing the mmap+memcpy read with two `esp_partition_read()` calls (which DO
take the mutex) — 35-minute soak test on real hardware, zero errors across
38,801 polls (previously reproduced the crash in ~10 minutes typical).

This aligns with upstream ESP-IDF commit `789ce684`
(`ESP_PARTITION_MMAP_BLOCKS_WRITE`), which addresses the same class of issue
for mmap'd reads racing flash writes.

Full root-cause writeup and testing history:
https://github.com/sslivins/arctic-controller/issues/210

This patch should be dropped once we upgrade to an ESP-IDF release that
includes an equivalent upstream fix for `read_otadata()` specifically (the
`789ce684` mechanism does not appear to cover this call site as of v5.5.2).
