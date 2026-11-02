# spi_flash override (ESP-IDF v6.1)

A copy of ESP-IDF v6.1's `components/spi_flash` (commit `fff9895c`, `test_apps`
removed). A project component with the same name replaces IDF's, so this is the
`spi_flash` the firmware builds with. Check with
`build/project_description.json` → `build_component_info.spi_flash.dir`.

## The one change

`flash_mmap.c`: `semphr_acq` is a counting semaphore instead of a binary one,
plus `#include <limits.h>`.

```c
s_mmap_lock.semphr_acq = xSemaphoreCreateCounting(INT_MAX, 0);
```

`mmap_lock_unfreeze()` gives `semphr_acq` once per mmap caller waiting behind a
flash write or erase. With a binary semaphore every give after the first
fails, so all but one of those callers sleep forever while still counted in
`acquired_count`. The next write or erase then waits for them to release and
never returns. On this device that hung OTA, history writes and restart
(TASK_WDT crashes in CI). A stress test on the dev unit deadlocked on the first
erase with stock IDF, and ran 600 s / ~25k erase-write cycles clean with this
change.

Upstream: [espressif/esp-idf#19351](https://github.com/espressif/esp-idf/pull/19351).

## Removing it

`idf_spi_flash_guard.cmake` fails the build when IDF's own `spi_flash` no
longer matches the v6.1 sources this was copied from (e.g. the floating
`espressif/idf:v6.1` image moves to a patch release).

Delete this directory once the IDF version in use contains the fix (or the
mmap lock is gone). When moving to a new IDF version before then, re-copy
`components/spi_flash` from that version, re-apply the change and update the
hash in the guard (the guard file says how to print it).
