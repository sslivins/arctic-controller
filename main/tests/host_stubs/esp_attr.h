#pragma once

// Host builds have no PSRAM: section placement attributes are no-ops.
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
