#include "history_storage.h"
#include "task_report.h"

#include <atomic>
#include <esp_crc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "history_storage";

static constexpr uint8_t HISTORY_PARTITION_SUBTYPE = 0x40;
static constexpr size_t FLASH_SECTOR_SIZE = 4096;
static constexpr size_t EVENT_BANK_SIZE = HISTORY_EVENT_REGION_SIZE / 2;
static constexpr size_t EVENT_RECORD_SIZE = 64;
static constexpr size_t EVENT_RECORDS_PER_BANK =
    (EVENT_BANK_SIZE - FLASH_SECTOR_SIZE) / EVENT_RECORD_SIZE;
static constexpr uint32_t BANK_MAGIC = 0x48424B31;    // HBK1
static constexpr uint32_t RECORD_MAGIC = 0x48455631;  // HEV1
static constexpr uint16_t FORMAT_VERSION = 1;
static constexpr uint16_t RECORD_KIND_UPSERT = 1;
static constexpr uint32_t BANK_COMMITTED = 0;
static constexpr size_t TELEMETRY_REGION_OFFSET = HISTORY_EVENT_REGION_SIZE;
static constexpr size_t TELEMETRY_SECTOR_HEADER_SIZE = 32;
// Format 2 (current, the only one written) and format 1 (still read, so the
// journal survives the upgrade; it ages out as the ring wraps).
static constexpr size_t TELEMETRY_RECORD_SIZE = 40;
static constexpr size_t TELEMETRY_RECORDS_PER_SECTOR =
    (FLASH_SECTOR_SIZE - TELEMETRY_SECTOR_HEADER_SIZE) / TELEMETRY_RECORD_SIZE;
static constexpr size_t TELEMETRY_V1_RECORD_SIZE = 32;
static constexpr size_t TELEMETRY_V1_RECORDS_PER_SECTOR =
    (FLASH_SECTOR_SIZE - TELEMETRY_SECTOR_HEADER_SIZE) / TELEMETRY_V1_RECORD_SIZE;
static constexpr uint32_t TELEMETRY_SECTOR_MAGIC = 0x48545331;  // HTS1
static constexpr uint32_t TELEMETRY_V1_RECORD_MAGIC = 0x48545231;  // HTR1
static constexpr uint16_t TELEMETRY_FORMAT_VERSION = 2;
static constexpr uint16_t TELEMETRY_V1_FORMAT_VERSION = 1;
static constexpr uint32_t TELEMETRY_COMMITTED = 0;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t committed;
    uint8_t reserved[16];
} event_bank_header_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t journal_sequence;
    uint32_t event_id;
    event_entry_t entry;
    uint8_t reserved[20];
    uint32_t crc32;
} event_journal_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t first_sequence;
    uint16_t record_size;
    uint16_t records_per_sector;
    uint32_t reserved;
    uint32_t crc32;
    uint32_t committed;
} telemetry_sector_header_t;

// Format 1 record (written by firmware up to 3.4). The tank reading and the average
// power live in what were once reserved words; unused words are left erased
// (0xFFFF), so a value only counts alongside its flag.
typedef struct {
    uint32_t timestamp;
    uint32_t sequence;
    int16_t inlet_deci_c;
    int16_t outlet_deci_c;
    int16_t setpoint_deci_c;
    uint8_t mode;
    uint8_t flags;
} telemetry_flash_sample_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    telemetry_flash_sample_t sample;
    int16_t tank_deci_c;
    uint16_t average_power_w;
    uint32_t crc32;
} telemetry_record_v1_t;

// Format 2 record. No per-record magic or duplicated sequence: the sector
// header identifies the format and the CRC covers the record. Spare bytes are
// left erased so a later field can be added the way tank and power were.
typedef struct {
    uint32_t sequence;
    uint32_t timestamp;
    int16_t inlet_deci_c;
    int16_t outlet_deci_c;
    int16_t setpoint_deci_c;
    uint8_t mode;
    uint8_t flags;
    int16_t tank_deci_c;
    uint16_t average_power_w;
    uint16_t average_heat_w;
    uint16_t average_cop_x100;
    int16_t outdoor_coil_deci_c;
    int16_t outdoor_air_deci_c;
    uint8_t flags2;
    uint8_t spare[7];
    uint32_t crc32;
} telemetry_record_t;

static_assert(sizeof(event_bank_header_t) == 32, "Unexpected event bank header size");
static_assert(sizeof(event_journal_record_t) == EVENT_RECORD_SIZE,
              "Event journal records must remain fixed-size");
static_assert(sizeof(telemetry_flash_sample_t) == 16,
              "Telemetry samples must remain compact");
static_assert(sizeof(telemetry_sector_header_t) == TELEMETRY_SECTOR_HEADER_SIZE,
              "Unexpected telemetry sector header size");
static_assert(sizeof(telemetry_record_v1_t) == TELEMETRY_V1_RECORD_SIZE,
              "Format 1 telemetry records are fixed on flash");
static_assert(sizeof(telemetry_record_t) == TELEMETRY_RECORD_SIZE,
              "Telemetry records must remain fixed-size");

static const esp_partition_t* s_partition = nullptr;
static int s_active_bank = -1;
static uint32_t s_generation = 0;
static uint32_t s_next_journal_sequence = 1;
static size_t s_next_record = 0;
static SemaphoreHandle_t s_telemetry_mutex = nullptr;
static bool s_factory_reset_pending = false;
static std::atomic_bool s_reboot_pending{false};
static bool s_telemetry_initialized = false;
static size_t s_telemetry_sector_count = 0;
static size_t s_telemetry_active_sector = 0;
static size_t s_telemetry_next_slot = 0;
static uint32_t s_telemetry_generation = 0;
static uint32_t s_telemetry_next_sequence = 1;
static uint32_t s_telemetry_last_timestamp = 0;

static size_t bank_offset(int bank) {
    return (size_t)bank * EVENT_BANK_SIZE;
}

static size_t record_offset(int bank, size_t record_index) {
    return bank_offset(bank) + FLASH_SECTOR_SIZE + (record_index * EVENT_RECORD_SIZE);
}

static bool bytes_are_erased(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0xFF) return false;
    }
    return true;
}

static uint32_t record_crc(const event_journal_record_t* record) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(record),
                        offsetof(event_journal_record_t, crc32));
}

static bool header_valid(const event_bank_header_t* header) {
    return header->magic == BANK_MAGIC &&
           header->version == FORMAT_VERSION &&
           header->header_size == sizeof(event_bank_header_t) &&
           header->committed == BANK_COMMITTED;
}

static bool record_valid(const event_journal_record_t* record) {
    return record->magic == RECORD_MAGIC &&
           record->version == FORMAT_VERSION &&
           record->kind == RECORD_KIND_UPSERT &&
           record->event_id != 0 &&
           record->entry.type < EVENT_TYPE_COUNT &&
           record->crc32 == record_crc(record);
}

static esp_err_t read_header(int bank, event_bank_header_t* header) {
    return esp_partition_read(s_partition, bank_offset(bank), header, sizeof(*header));
}

static esp_err_t write_record(int bank,
                              size_t record_index,
                              uint32_t journal_sequence,
                              uint32_t event_id,
                              const event_entry_t* entry) {
    event_journal_record_t record;
    memset(&record, 0xFF, sizeof(record));
    record.magic = RECORD_MAGIC;
    record.version = FORMAT_VERSION;
    record.kind = RECORD_KIND_UPSERT;
    record.journal_sequence = journal_sequence;
    record.event_id = event_id;
    record.entry = *entry;
    record.crc32 = record_crc(&record);
    return esp_partition_write(s_partition, record_offset(bank, record_index),
                               &record, sizeof(record));
}

static esp_err_t commit_bank(int bank, uint32_t generation) {
    event_bank_header_t header;
    memset(&header, 0xFF, sizeof(header));
    header.magic = BANK_MAGIC;
    header.version = FORMAT_VERSION;
    header.header_size = sizeof(header);
    header.generation = generation;

    esp_err_t err = esp_partition_write(s_partition, bank_offset(bank),
                                        &header, sizeof(header));
    if (err != ESP_OK) return err;

    uint32_t committed = BANK_COMMITTED;
    return esp_partition_write(s_partition,
                               bank_offset(bank) + offsetof(event_bank_header_t, committed),
                               &committed, sizeof(committed));
}

static esp_err_t initialize_empty_bank(void) {
    esp_err_t err = esp_partition_erase_range(s_partition, 0, EVENT_BANK_SIZE);
    if (err != ESP_OK) return err;
    err = commit_bank(0, 1);
    if (err != ESP_OK) return err;

    s_active_bank = 0;
    s_generation = 1;
    s_next_journal_sequence = 1;
    s_next_record = 0;
    return ESP_OK;
}

esp_err_t history_storage_init(void) {
    if (s_partition != nullptr) return ESP_OK;

    if (s_telemetry_mutex == nullptr) {
        s_telemetry_mutex = xSemaphoreCreateMutex();
        if (s_telemetry_mutex == nullptr) {
            ESP_LOGE(TAG, "Failed to create telemetry storage mutex");
            return ESP_ERR_NO_MEM;
        }
        task_report_register_lock("telemetry storage", s_telemetry_mutex);
    }

    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(HISTORY_PARTITION_SUBTYPE),
        "history");
    if (s_partition == nullptr) {
        ESP_LOGE(TAG, "History partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_partition->size < HISTORY_EVENT_REGION_SIZE) {
        ESP_LOGE(TAG, "History partition is too small: %lu bytes",
                 (unsigned long)s_partition->size);
        s_partition = nullptr;
        return ESP_ERR_INVALID_SIZE;
    }

    event_bank_header_t headers[2];
    bool valid[2] = {};
    for (int bank = 0; bank < 2; bank++) {
        esp_err_t err = read_header(bank, &headers[bank]);
        if (err != ESP_OK) {
            s_partition = nullptr;
            return err;
        }
        valid[bank] = header_valid(&headers[bank]);
    }

    if (!valid[0] && !valid[1]) {
        ESP_LOGI(TAG, "Initializing empty event journal");
        return initialize_empty_bank();
    }

    if (valid[0] && valid[1]) {
        s_active_bank = headers[1].generation > headers[0].generation ? 1 : 0;
    } else {
        s_active_bank = valid[0] ? 0 : 1;
    }
    s_generation = headers[s_active_bank].generation;
    return ESP_OK;
}

esp_err_t history_storage_load_events(event_entry_t* entries,
                                      uint32_t* event_ids,
                                      size_t capacity,
                                      size_t* count,
                                      uint32_t* next_event_id) {
    if (entries == nullptr || event_ids == nullptr || count == nullptr ||
        next_event_id == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = history_storage_init();
    if (err != ESP_OK) return err;

    *count = 0;
    *next_event_id = 1;
    s_next_journal_sequence = 1;
    s_next_record = 0;
    bool damaged_tail = false;

    for (size_t slot = 0; slot < EVENT_RECORDS_PER_BANK; slot++) {
        event_journal_record_t record;
        err = esp_partition_read(s_partition, record_offset(s_active_bank, slot),
                                 &record, sizeof(record));
        if (err != ESP_OK) return err;

        if (bytes_are_erased(&record, sizeof(record))) {
            s_next_record = slot;
            break;
        }
        if (!record_valid(&record)) {
            ESP_LOGW(TAG, "Ignoring incomplete/corrupt journal tail at slot %lu",
                     (unsigned long)slot);
            s_next_record = slot;
            damaged_tail = true;
            break;
        }

        size_t existing = *count;
        for (size_t i = 0; i < *count; i++) {
            if (event_ids[i] == record.event_id) {
                existing = i;
                break;
            }
        }

        if (existing < *count) {
            entries[existing] = record.entry;
        } else if (*count < capacity) {
            event_ids[*count] = record.event_id;
            entries[*count] = record.entry;
            (*count)++;
        } else {
            memmove(entries, entries + 1, (capacity - 1) * sizeof(*entries));
            memmove(event_ids, event_ids + 1, (capacity - 1) * sizeof(*event_ids));
            event_ids[capacity - 1] = record.event_id;
            entries[capacity - 1] = record.entry;
        }

        if (record.event_id >= *next_event_id) {
            *next_event_id = record.event_id + 1;
            if (*next_event_id == 0) *next_event_id = 1;
        }
        if (record.journal_sequence >= s_next_journal_sequence) {
            s_next_journal_sequence = record.journal_sequence + 1;
            if (s_next_journal_sequence == 0) s_next_journal_sequence = 1;
        }
        s_next_record = slot + 1;
    }

    if (damaged_tail) {
        err = history_storage_replace_events(entries, event_ids, capacity, *count, *count);
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "Loaded %lu event(s), bank %d generation %lu, slot %lu",
             (unsigned long)*count, s_active_bank, (unsigned long)s_generation,
             (unsigned long)s_next_record);
    return ESP_OK;
}

esp_err_t history_storage_append_event(uint32_t event_id, const event_entry_t* entry) {
    if (event_id == 0 || entry == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_reboot_pending.load()) return ESP_ERR_INVALID_STATE;
    if (s_partition == nullptr || s_active_bank < 0) return ESP_ERR_INVALID_STATE;
    if (s_next_record >= EVENT_RECORDS_PER_BANK) return ESP_ERR_NO_MEM;

    esp_err_t err = write_record(s_active_bank, s_next_record,
                                 s_next_journal_sequence, event_id, entry);
    if (err == ESP_OK) {
        s_next_record++;
        s_next_journal_sequence++;
        if (s_next_journal_sequence == 0) s_next_journal_sequence = 1;
    }
    return err;
}

esp_err_t history_storage_replace_events(const event_entry_t* entries,
                                         const uint32_t* event_ids,
                                         size_t capacity,
                                         size_t head,
                                         size_t count) {
    if (s_reboot_pending.load()) return ESP_ERR_INVALID_STATE;
    if (s_partition == nullptr || s_active_bank < 0) return ESP_ERR_INVALID_STATE;
    if (count > capacity || count > EVENT_RECORDS_PER_BANK ||
        (count > 0 && (entries == nullptr || event_ids == nullptr))) {
        return ESP_ERR_INVALID_ARG;
    }

    const int replacement_bank = 1 - s_active_bank;
    const uint32_t replacement_generation = s_generation + 1;
    esp_err_t err = esp_partition_erase_range(
        s_partition, bank_offset(replacement_bank), EVENT_BANK_SIZE);
    if (err != ESP_OK) return err;

    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        size_t index = (head + capacity - count + i) % capacity;
        err = write_record(replacement_bank, written, s_next_journal_sequence,
                           event_ids[index], &entries[index]);
        if (err != ESP_OK) return err;
        written++;
        s_next_journal_sequence++;
        if (s_next_journal_sequence == 0) s_next_journal_sequence = 1;
    }

    err = commit_bank(replacement_bank, replacement_generation);
    if (err != ESP_OK) return err;

    s_active_bank = replacement_bank;
    s_generation = replacement_generation;
    s_next_record = written;
    ESP_LOGI(TAG, "Compacted %lu event(s) into bank %d generation %lu",
             (unsigned long)count, s_active_bank, (unsigned long)s_generation);
    return ESP_OK;
}

static size_t telemetry_sector_offset(size_t sector) {
    return TELEMETRY_REGION_OFFSET + sector * FLASH_SECTOR_SIZE;
}

// Offset of a slot in a format 2 sector (the only format written).
static size_t telemetry_record_offset(size_t sector, size_t slot) {
    return telemetry_sector_offset(sector) + TELEMETRY_SECTOR_HEADER_SIZE +
           slot * TELEMETRY_RECORD_SIZE;
}

static uint32_t telemetry_header_crc(const telemetry_sector_header_t* header) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(header),
                        offsetof(telemetry_sector_header_t, crc32));
}

static uint32_t telemetry_record_crc(const telemetry_record_t* record) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(record),
                        offsetof(telemetry_record_t, crc32));
}

static uint32_t telemetry_v1_record_crc(const telemetry_record_v1_t* record) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(record),
                        offsetof(telemetry_record_v1_t, crc32));
}

// The record format a committed sector holds, or 0 if it holds nothing this
// firmware can read. Older firmware accepts only format 1 sectors, so after a
// rollback it skips format 2 ones and reuses them as its ring wraps.
static uint16_t telemetry_sector_format(const telemetry_sector_header_t* header) {
    if (header->magic != TELEMETRY_SECTOR_MAGIC ||
        header->header_size != TELEMETRY_SECTOR_HEADER_SIZE ||
        header->committed != TELEMETRY_COMMITTED ||
        header->crc32 != telemetry_header_crc(header)) {
        return 0;
    }
    if (header->version == TELEMETRY_FORMAT_VERSION &&
        header->record_size == TELEMETRY_RECORD_SIZE &&
        header->records_per_sector == TELEMETRY_RECORDS_PER_SECTOR) {
        return TELEMETRY_FORMAT_VERSION;
    }
    if (header->version == TELEMETRY_V1_FORMAT_VERSION &&
        header->record_size == TELEMETRY_V1_RECORD_SIZE &&
        header->records_per_sector == TELEMETRY_V1_RECORDS_PER_SECTOR) {
        return TELEMETRY_V1_FORMAT_VERSION;
    }
    return 0;
}

static size_t telemetry_slots_in(uint16_t format) {
    return format == TELEMETRY_FORMAT_VERSION ? TELEMETRY_RECORDS_PER_SECTOR
                                              : TELEMETRY_V1_RECORDS_PER_SECTOR;
}

static size_t telemetry_record_size_of(uint16_t format) {
    return format == TELEMETRY_FORMAT_VERSION ? TELEMETRY_RECORD_SIZE
                                              : TELEMETRY_V1_RECORD_SIZE;
}

static bool generation_newer(uint32_t candidate, uint32_t current) {
    return static_cast<int32_t>(candidate - current) > 0;
}

static esp_err_t telemetry_commit_sector(size_t sector,
                                         uint32_t generation,
                                         uint32_t first_sequence) {
    esp_err_t err = esp_partition_erase_range(
        s_partition, telemetry_sector_offset(sector), FLASH_SECTOR_SIZE);
    if (err != ESP_OK) return err;

    telemetry_sector_header_t header;
    memset(&header, 0xFF, sizeof(header));
    header.magic = TELEMETRY_SECTOR_MAGIC;
    header.version = TELEMETRY_FORMAT_VERSION;
    header.header_size = TELEMETRY_SECTOR_HEADER_SIZE;
    header.generation = generation;
    header.first_sequence = first_sequence;
    header.record_size = TELEMETRY_RECORD_SIZE;
    header.records_per_sector = TELEMETRY_RECORDS_PER_SECTOR;
    header.reserved = 0xFFFFFFFF;
    header.crc32 = telemetry_header_crc(&header);

    err = esp_partition_write(s_partition, telemetry_sector_offset(sector),
                              &header, sizeof(header));
    if (err != ESP_OK) return err;

    const uint32_t committed = TELEMETRY_COMMITTED;
    return esp_partition_write(
        s_partition,
        telemetry_sector_offset(sector) +
            offsetof(telemetry_sector_header_t, committed),
        &committed, sizeof(committed));
}

static void pack_telemetry_record(telemetry_record_t* record,
                                  const history_telemetry_sample_t* sample,
                                  uint32_t sequence) {
    memset(record, 0xFF, sizeof(*record));
    record->sequence = sequence;
    record->timestamp = sample->timestamp;
    record->inlet_deci_c = sample->inlet_deci_c;
    record->outlet_deci_c = sample->outlet_deci_c;
    record->setpoint_deci_c = sample->setpoint_deci_c;
    record->mode = sample->mode;
    uint8_t flags = sample->flags;
    uint8_t flags2 = sample->flags2 &
                     (HISTORY_TELEMETRY_HEAT_VALID | HISTORY_TELEMETRY_COP_VALID |
                      HISTORY_TELEMETRY_OUTDOOR_COIL_VALID |
                      HISTORY_TELEMETRY_OUTDOOR_AIR_VALID);
    if (flags & HISTORY_TELEMETRY_TANK_VALID) {
        record->tank_deci_c = sample->tank_deci_c;
    }
    // 0xFFFF is the erased marker, so it can never be stored as a reading.
    if ((flags & HISTORY_TELEMETRY_POWER_VALID) && sample->average_power_w != 0xFFFF) {
        record->average_power_w = sample->average_power_w;
    } else {
        flags &= (uint8_t)~HISTORY_TELEMETRY_POWER_VALID;
    }
    if ((flags2 & HISTORY_TELEMETRY_HEAT_VALID) && sample->average_heat_w != 0xFFFF) {
        record->average_heat_w = sample->average_heat_w;
    } else {
        flags2 &= (uint8_t)~HISTORY_TELEMETRY_HEAT_VALID;
    }
    if ((flags2 & HISTORY_TELEMETRY_COP_VALID) && sample->average_cop_x100 != 0xFFFF) {
        record->average_cop_x100 = sample->average_cop_x100;
    } else {
        flags2 &= (uint8_t)~HISTORY_TELEMETRY_COP_VALID;
    }
    if (flags2 & HISTORY_TELEMETRY_OUTDOOR_COIL_VALID) {
        record->outdoor_coil_deci_c = sample->outdoor_coil_deci_c;
    }
    if (flags2 & HISTORY_TELEMETRY_OUTDOOR_AIR_VALID) {
        record->outdoor_air_deci_c = sample->outdoor_air_deci_c;
    }
    record->flags = flags;
    record->flags2 = flags2;
    record->crc32 = telemetry_record_crc(record);
}

static bool decode_v1_record(const uint8_t* bytes, history_telemetry_sample_t* out) {
    telemetry_record_v1_t record;
    memcpy(&record, bytes, sizeof(record));
    if (record.magic != TELEMETRY_V1_RECORD_MAGIC || record.sequence == 0 ||
        record.sample.sequence != record.sequence ||
        record.sample.mode > HISTORY_TELEMETRY_MODE_HOT_WATER ||
        record.crc32 != telemetry_v1_record_crc(&record)) {
        return false;
    }
    history_telemetry_sample_t sample = {};
    sample.timestamp = record.sample.timestamp;
    sample.sequence = record.sample.sequence;
    sample.inlet_deci_c = record.sample.inlet_deci_c;
    sample.outlet_deci_c = record.sample.outlet_deci_c;
    sample.setpoint_deci_c = record.sample.setpoint_deci_c;
    sample.mode = record.sample.mode;
    sample.flags = record.sample.flags;
    if (sample.flags & HISTORY_TELEMETRY_TANK_VALID) {
        sample.tank_deci_c = record.tank_deci_c;
    }
    if ((sample.flags & HISTORY_TELEMETRY_POWER_VALID) && record.average_power_w != 0xFFFF) {
        sample.average_power_w = record.average_power_w;
    } else {
        sample.flags &= (uint8_t)~HISTORY_TELEMETRY_POWER_VALID;
    }
    *out = sample;
    return true;
}

static bool decode_v2_record(const uint8_t* bytes, history_telemetry_sample_t* out) {
    telemetry_record_t record;
    memcpy(&record, bytes, sizeof(record));
    if (record.sequence == 0 || record.sequence == 0xFFFFFFFF ||
        record.mode > HISTORY_TELEMETRY_MODE_HOT_WATER ||
        record.crc32 != telemetry_record_crc(&record)) {
        return false;
    }
    history_telemetry_sample_t sample = {};
    sample.timestamp = record.timestamp;
    sample.sequence = record.sequence;
    sample.inlet_deci_c = record.inlet_deci_c;
    sample.outlet_deci_c = record.outlet_deci_c;
    sample.setpoint_deci_c = record.setpoint_deci_c;
    sample.mode = record.mode;
    sample.flags = record.flags;
    sample.flags2 = record.flags2;
    if (sample.flags & HISTORY_TELEMETRY_TANK_VALID) {
        sample.tank_deci_c = record.tank_deci_c;
    }
    if ((sample.flags & HISTORY_TELEMETRY_POWER_VALID) && record.average_power_w != 0xFFFF) {
        sample.average_power_w = record.average_power_w;
    } else {
        sample.flags &= (uint8_t)~HISTORY_TELEMETRY_POWER_VALID;
    }
    if ((sample.flags2 & HISTORY_TELEMETRY_HEAT_VALID) && record.average_heat_w != 0xFFFF) {
        sample.average_heat_w = record.average_heat_w;
    } else {
        sample.flags2 &= (uint8_t)~HISTORY_TELEMETRY_HEAT_VALID;
    }
    if ((sample.flags2 & HISTORY_TELEMETRY_COP_VALID) && record.average_cop_x100 != 0xFFFF) {
        sample.average_cop_x100 = record.average_cop_x100;
    } else {
        sample.flags2 &= (uint8_t)~HISTORY_TELEMETRY_COP_VALID;
    }
    if (sample.flags2 & HISTORY_TELEMETRY_OUTDOOR_COIL_VALID) {
        sample.outdoor_coil_deci_c = record.outdoor_coil_deci_c;
    }
    if (sample.flags2 & HISTORY_TELEMETRY_OUTDOOR_AIR_VALID) {
        sample.outdoor_air_deci_c = record.outdoor_air_deci_c;
    }
    *out = sample;
    return true;
}

// Decodes one slot of a sector image read into RAM. False for an erased, torn
// or corrupt slot.
static bool telemetry_decode_slot(uint16_t format, const uint8_t* sector_data,
                                  size_t slot, history_telemetry_sample_t* out) {
    const size_t size = telemetry_record_size_of(format);
    const uint8_t* bytes = sector_data + TELEMETRY_SECTOR_HEADER_SIZE + slot * size;
    if (bytes_are_erased(bytes, size)) return false;
    return format == TELEMETRY_FORMAT_VERSION ? decode_v2_record(bytes, out)
                                              : decode_v1_record(bytes, out);
}

static esp_err_t telemetry_rotate_locked() {
    const size_t next_sector =
        (s_telemetry_active_sector + 1) % s_telemetry_sector_count;
    uint32_t next_generation = s_telemetry_generation + 1;
    if (next_generation == 0) next_generation = 1;
    esp_err_t err = telemetry_commit_sector(
        next_sector, next_generation, s_telemetry_next_sequence);
    if (err != ESP_OK) return err;
    s_telemetry_active_sector = next_sector;
    s_telemetry_generation = next_generation;
    s_telemetry_next_slot = 0;
    return ESP_OK;
}

static esp_err_t telemetry_initialize_locked() {
    if (s_factory_reset_pending) return ESP_ERR_INVALID_STATE;
    if (s_telemetry_initialized) return ESP_OK;

    esp_err_t err = history_storage_init();
    if (err != ESP_OK) return err;
    if (s_partition->size <= TELEMETRY_REGION_OFFSET) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_telemetry_sector_count =
        (s_partition->size - TELEMETRY_REGION_OFFSET) / FLASH_SECTOR_SIZE;
    if (s_telemetry_sector_count == 0) return ESP_ERR_INVALID_SIZE;

    auto* sector_data = static_cast<uint8_t*>(
        heap_caps_malloc(FLASH_SECTOR_SIZE, MALLOC_CAP_8BIT));
    if (sector_data == nullptr) return ESP_ERR_NO_MEM;

    // The active sector is the one holding the newest record, not simply the
    // highest generation: firmware rolled back to format 1 numbers its new
    // sectors from its own (lower) generation, so after re-upgrading, the
    // highest-generation sector can be an old format 2 one. Ties (empty
    // sectors) go to the newer generation.
    bool found_header = false;
    size_t active_sector = 0;
    uint16_t active_format = 0;
    uint32_t active_newest = 0;
    uint32_t active_generation = 0;
    size_t active_next_slot = 0;
    uint32_t max_generation = 0;
    uint32_t next_sequence = 1;
    uint32_t last_timestamp = 0;

    for (size_t sector = 0; sector < s_telemetry_sector_count; sector++) {
        err = esp_partition_read(s_partition, telemetry_sector_offset(sector),
                                 sector_data, FLASH_SECTOR_SIZE);
        if (err != ESP_OK) break;
        telemetry_sector_header_t header;
        memcpy(&header, sector_data, sizeof(header));
        const uint16_t format = telemetry_sector_format(&header);
        if (format == 0) continue;

        if (!found_header || generation_newer(header.generation, max_generation)) {
            max_generation = header.generation;
        }
        if (header.first_sequence > next_sequence) next_sequence = header.first_sequence;

        const size_t slots = telemetry_slots_in(format);
        const size_t size = telemetry_record_size_of(format);
        uint32_t sector_newest = 0;
        size_t last_programmed_slot = 0;
        bool any_programmed = false;
        for (size_t slot = 0; slot < slots; slot++) {
            if (!bytes_are_erased(sector_data + TELEMETRY_SECTOR_HEADER_SIZE + slot * size,
                                  size)) {
                any_programmed = true;
                last_programmed_slot = slot;
            }
            history_telemetry_sample_t sample;
            if (!telemetry_decode_slot(format, sector_data, slot, &sample)) continue;
            if (sample.sequence >= next_sequence) {
                next_sequence = sample.sequence + 1;
                if (next_sequence == 0) next_sequence = 1;
            }
            if (sample.timestamp > sector_newest) sector_newest = sample.timestamp;
        }
        if (sector_newest > last_timestamp) last_timestamp = sector_newest;

        if (!found_header || sector_newest > active_newest ||
            (sector_newest == active_newest &&
             generation_newer(header.generation, active_generation))) {
            active_sector = sector;
            active_format = format;
            active_newest = sector_newest;
            active_generation = header.generation;
            active_next_slot = any_programmed ? last_programmed_slot + 1 : 0;
        }
        found_header = true;
    }
    heap_caps_free(sector_data);
    if (err != ESP_OK) return err;

    if (!found_header) {
        err = telemetry_commit_sector(0, 1, 1);
        if (err != ESP_OK) return err;
        s_telemetry_active_sector = 0;
        s_telemetry_generation = 1;
        s_telemetry_next_sequence = 1;
        s_telemetry_next_slot = 0;
        s_telemetry_last_timestamp = 0;
        s_telemetry_initialized = true;
        ESP_LOGI(TAG, "Initialized telemetry journal (%lu sectors)",
                 (unsigned long)s_telemetry_sector_count);
        return ESP_OK;
    }

    s_telemetry_active_sector = active_sector;
    // Rotation numbers the next sector above every generation on flash.
    s_telemetry_generation = max_generation;
    s_telemetry_next_sequence = next_sequence;
    s_telemetry_next_slot = active_next_slot;
    s_telemetry_last_timestamp = last_timestamp;

    // Only the current format is ever appended: leave an older-format active
    // sector as it is and continue in a fresh one.
    if (active_format != TELEMETRY_FORMAT_VERSION) {
        err = telemetry_rotate_locked();
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "Telemetry journal upgraded to format %u at sector %lu",
                 (unsigned)TELEMETRY_FORMAT_VERSION,
                 (unsigned long)s_telemetry_active_sector);
    }

    s_telemetry_initialized = true;
    ESP_LOGI(TAG,
             "Recovered telemetry journal: sector=%lu generation=%lu slot=%lu sequence=%lu",
             (unsigned long)s_telemetry_active_sector,
             (unsigned long)s_telemetry_generation,
             (unsigned long)s_telemetry_next_slot,
             (unsigned long)s_telemetry_next_sequence);
    return ESP_OK;
}

esp_err_t history_storage_append_telemetry(
    const history_telemetry_sample_t* sample) {
    if (sample == nullptr || sample->timestamp == 0) return ESP_ERR_INVALID_ARG;
    if (s_reboot_pending.load()) return ESP_ERR_INVALID_STATE;
    if (s_telemetry_mutex == nullptr) {
        esp_err_t err = history_storage_init();
        if (err != ESP_OK) return err;
    }

    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    esp_err_t err = telemetry_initialize_locked();
    if (err != ESP_OK) {
        xSemaphoreGive(s_telemetry_mutex);
        return err;
    }
    if (sample->timestamp <= s_telemetry_last_timestamp) {
        xSemaphoreGive(s_telemetry_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t attempts = 0; attempts < 2; attempts++) {
        if (s_telemetry_next_slot >= TELEMETRY_RECORDS_PER_SECTOR) {
            err = telemetry_rotate_locked();
            if (err != ESP_OK) break;
        }

        telemetry_record_t record;
        pack_telemetry_record(&record, sample, s_telemetry_next_sequence);

        err = esp_partition_write(
            s_partition,
            telemetry_record_offset(s_telemetry_active_sector,
                                    s_telemetry_next_slot),
            &record, sizeof(record));
        s_telemetry_next_slot++;
        if (err == ESP_OK) {
            s_telemetry_last_timestamp = sample->timestamp;
            s_telemetry_next_sequence++;
            if (s_telemetry_next_sequence == 0) s_telemetry_next_sequence = 1;
            break;
        }
        ESP_LOGW(TAG, "Telemetry write failed at sector %lu slot %lu: %s",
                 (unsigned long)s_telemetry_active_sector,
                 (unsigned long)(s_telemetry_next_slot - 1),
                 esp_err_to_name(err));
    }

    xSemaphoreGive(s_telemetry_mutex);
    return err;
}

static int telemetry_sample_compare(const void* lhs, const void* rhs) {
    const auto* a = static_cast<const history_telemetry_sample_t*>(lhs);
    const auto* b = static_cast<const history_telemetry_sample_t*>(rhs);
    if (a->timestamp != b->timestamp) return a->timestamp < b->timestamp ? -1 : 1;
    if (a->sequence == b->sequence) return 0;
    return a->sequence < b->sequence ? -1 : 1;
}

esp_err_t history_storage_query_telemetry(
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    history_telemetry_sample_t* samples,
    size_t capacity,
    size_t* count) {
    if (samples == nullptr || count == nullptr || capacity == 0 ||
        start_timestamp >= end_timestamp) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_telemetry_mutex == nullptr) {
        esp_err_t err = history_storage_init();
        if (err != ESP_OK) return err;
    }

    *count = 0;
    auto* sector_data = static_cast<uint8_t*>(
        heap_caps_malloc(FLASH_SECTOR_SIZE, MALLOC_CAP_8BIT));
    if (sector_data == nullptr) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    esp_err_t err = telemetry_initialize_locked();
    if (err != ESP_OK) {
        xSemaphoreGive(s_telemetry_mutex);
        heap_caps_free(sector_data);
        return err;
    }

    for (size_t sector = 0; sector < s_telemetry_sector_count; sector++) {
        telemetry_sector_header_t header;
        err = esp_partition_read(s_partition, telemetry_sector_offset(sector),
                                 sector_data, FLASH_SECTOR_SIZE);
        if (err != ESP_OK) break;
        memcpy(&header, sector_data, sizeof(header));
        const uint16_t format = telemetry_sector_format(&header);
        if (format == 0) continue;

        const size_t slots = telemetry_slots_in(format);
        for (size_t slot = 0; slot < slots; slot++) {
            history_telemetry_sample_t sample;
            if (!telemetry_decode_slot(format, sector_data, slot, &sample)) continue;
            if (sample.timestamp < start_timestamp ||
                sample.timestamp >= end_timestamp) {
                continue;
            }
            if (*count < capacity) {
                samples[(*count)++] = sample;
            } else {
                size_t oldest = 0;
                for (size_t i = 1; i < *count; i++) {
                    if (telemetry_sample_compare(&samples[i], &samples[oldest]) < 0) {
                        oldest = i;
                    }
                }
                if (telemetry_sample_compare(&sample, &samples[oldest]) > 0) {
                    samples[oldest] = sample;
                }
            }
        }
        if (err != ESP_OK) break;
    }

    if (err == ESP_OK && *count > 1) {
        qsort(samples, *count, sizeof(*samples), telemetry_sample_compare);
    }
    xSemaphoreGive(s_telemetry_mutex);
    heap_caps_free(sector_data);
    return err;
}

esp_err_t history_storage_latest_telemetry_timestamp(uint32_t* timestamp) {
    if (timestamp == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_telemetry_mutex == nullptr) {
        esp_err_t err = history_storage_init();
        if (err != ESP_OK) return err;
    }
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    esp_err_t err = telemetry_initialize_locked();
    if (err == ESP_OK) *timestamp = s_telemetry_last_timestamp;
    xSemaphoreGive(s_telemetry_mutex);
    return err;
}

void history_storage_prepare_factory_reset(void) {
    if (s_telemetry_mutex == nullptr && history_storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize telemetry lock for factory reset");
        return;
    }
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    s_factory_reset_pending = true;
    xSemaphoreGive(s_telemetry_mutex);
}

void history_storage_begin_reboot(void) {
    // Block any new event/telemetry flash write. Set the gate first so callers
    // that check it (see the write entry points) bail before touching flash.
    s_reboot_pending.store(true);
    // Then wait out an in-flight telemetry write (the only path that holds the
    // mutex across an esp_partition op) so no SPI-flash operation is running
    // when the caller invokes esp_restart().
    if (s_telemetry_mutex != nullptr) {
        if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            xSemaphoreGive(s_telemetry_mutex);
        }
    }
}

#ifdef CONFIG_TEST_ENDPOINTS
esp_err_t history_storage_seed_telemetry_for_test(
    const history_telemetry_sample_t* samples,
    size_t count) {
    if (samples == nullptr || count == 0) return ESP_ERR_INVALID_ARG;
    if (s_telemetry_mutex == nullptr) {
        esp_err_t err = history_storage_init();
        if (err != ESP_OK) return err;
    }
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);

    s_factory_reset_pending = false;
    s_telemetry_initialized = false;
    esp_err_t err = esp_partition_erase_range(
        s_partition, TELEMETRY_REGION_OFFSET,
        s_partition->size - TELEMETRY_REGION_OFFSET);
    if (err == ESP_OK) {
        err = telemetry_commit_sector(0, 1, 1);
    }
    if (err == ESP_OK) {
        s_telemetry_sector_count =
            (s_partition->size - TELEMETRY_REGION_OFFSET) / FLASH_SECTOR_SIZE;
        s_telemetry_active_sector = 0;
        s_telemetry_generation = 1;
        s_telemetry_next_slot = 0;
        s_telemetry_next_sequence = 1;
        s_telemetry_last_timestamp = 0;
        s_telemetry_initialized = true;
    }

    for (size_t i = 0; err == ESP_OK && i < count; i++) {
        if (s_telemetry_next_slot >= TELEMETRY_RECORDS_PER_SECTOR) {
            err = telemetry_rotate_locked();
            if (err != ESP_OK) break;
        }
        telemetry_record_t record;
        pack_telemetry_record(&record, &samples[i], s_telemetry_next_sequence);
        err = esp_partition_write(
            s_partition,
            telemetry_record_offset(s_telemetry_active_sector,
                                    s_telemetry_next_slot),
            &record, sizeof(record));
        if (err == ESP_OK) {
            s_telemetry_next_slot++;
            s_telemetry_next_sequence++;
            s_telemetry_last_timestamp = samples[i].timestamp;
        }
    }
    xSemaphoreGive(s_telemetry_mutex);
    return err;
}
#endif
