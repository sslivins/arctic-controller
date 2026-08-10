#include "history_storage.h"

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
static constexpr size_t TELEMETRY_RECORD_SIZE = 32;
static constexpr size_t TELEMETRY_RECORDS_PER_SECTOR =
    (FLASH_SECTOR_SIZE - TELEMETRY_SECTOR_HEADER_SIZE) / TELEMETRY_RECORD_SIZE;
static constexpr uint32_t TELEMETRY_SECTOR_MAGIC = 0x48545331;  // HTS1
static constexpr uint32_t TELEMETRY_RECORD_MAGIC = 0x48545231;  // HTR1
static constexpr uint16_t TELEMETRY_FORMAT_VERSION = 1;
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

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    history_telemetry_sample_t sample;
    uint32_t reserved;
    uint32_t crc32;
} telemetry_record_t;

static_assert(sizeof(event_bank_header_t) == 32, "Unexpected event bank header size");
static_assert(sizeof(event_journal_record_t) == EVENT_RECORD_SIZE,
              "Event journal records must remain fixed-size");
static_assert(sizeof(history_telemetry_sample_t) == 16,
              "Telemetry samples must remain compact");
static_assert(sizeof(telemetry_sector_header_t) == TELEMETRY_SECTOR_HEADER_SIZE,
              "Unexpected telemetry sector header size");
static_assert(sizeof(telemetry_record_t) == TELEMETRY_RECORD_SIZE,
              "Telemetry records must remain fixed-size");

static const esp_partition_t* s_partition = nullptr;
static int s_active_bank = -1;
static uint32_t s_generation = 0;
static uint32_t s_next_journal_sequence = 1;
static size_t s_next_record = 0;
static SemaphoreHandle_t s_telemetry_mutex = nullptr;
static bool s_factory_reset_pending = false;
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

static bool telemetry_header_valid(const telemetry_sector_header_t* header) {
    return header->magic == TELEMETRY_SECTOR_MAGIC &&
           header->version == TELEMETRY_FORMAT_VERSION &&
           header->header_size == TELEMETRY_SECTOR_HEADER_SIZE &&
           header->record_size == TELEMETRY_RECORD_SIZE &&
           header->records_per_sector == TELEMETRY_RECORDS_PER_SECTOR &&
           header->committed == TELEMETRY_COMMITTED &&
           header->crc32 == telemetry_header_crc(header);
}

static bool telemetry_record_valid(const telemetry_record_t* record) {
    return record->magic == TELEMETRY_RECORD_MAGIC &&
           record->sequence != 0 &&
           record->sample.sequence == record->sequence &&
           record->sample.mode <= HISTORY_TELEMETRY_MODE_HOT_WATER &&
           record->crc32 == telemetry_record_crc(record);
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

    bool found_header = false;
    telemetry_sector_header_t newest = {};
    for (size_t sector = 0; sector < s_telemetry_sector_count; sector++) {
        telemetry_sector_header_t header;
        err = esp_partition_read(s_partition, telemetry_sector_offset(sector),
                                 &header, sizeof(header));
        if (err != ESP_OK) return err;
        if (!telemetry_header_valid(&header)) continue;
        if (!found_header || generation_newer(header.generation, newest.generation)) {
            newest = header;
            s_telemetry_active_sector = sector;
            found_header = true;
        }
    }

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

    s_telemetry_generation = newest.generation;
    s_telemetry_next_sequence = newest.first_sequence;
    s_telemetry_next_slot = 0;
    s_telemetry_last_timestamp = 0;

    for (size_t sector = 0; sector < s_telemetry_sector_count; sector++) {
        telemetry_sector_header_t header;
        err = esp_partition_read(s_partition, telemetry_sector_offset(sector),
                                 &header, sizeof(header));
        if (err != ESP_OK) return err;
        if (!telemetry_header_valid(&header)) continue;

        size_t last_programmed_slot = 0;
        bool any_programmed = false;
        for (size_t slot = 0; slot < TELEMETRY_RECORDS_PER_SECTOR; slot++) {
            telemetry_record_t record;
            err = esp_partition_read(s_partition,
                                     telemetry_record_offset(sector, slot),
                                     &record, sizeof(record));
            if (err != ESP_OK) return err;
            if (!bytes_are_erased(&record, sizeof(record))) {
                any_programmed = true;
                last_programmed_slot = slot;
            }
            if (!telemetry_record_valid(&record)) continue;
            if (record.sequence >= s_telemetry_next_sequence) {
                s_telemetry_next_sequence = record.sequence + 1;
                if (s_telemetry_next_sequence == 0) s_telemetry_next_sequence = 1;
            }
            if (record.sample.timestamp > s_telemetry_last_timestamp) {
                s_telemetry_last_timestamp = record.sample.timestamp;
            }
        }
        if (sector == s_telemetry_active_sector) {
            s_telemetry_next_slot =
                any_programmed ? last_programmed_slot + 1 : 0;
        }
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

esp_err_t history_storage_append_telemetry(
    const history_telemetry_sample_t* sample) {
    if (sample == nullptr || sample->timestamp == 0) return ESP_ERR_INVALID_ARG;
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
        memset(&record, 0xFF, sizeof(record));
        record.magic = TELEMETRY_RECORD_MAGIC;
        record.sequence = s_telemetry_next_sequence;
        record.sample = *sample;
        record.sample.sequence = s_telemetry_next_sequence;
        record.reserved = 0xFFFFFFFF;
        record.crc32 = telemetry_record_crc(&record);

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
        if (!telemetry_header_valid(&header)) continue;

        for (size_t slot = 0; slot < TELEMETRY_RECORDS_PER_SECTOR; slot++) {
            telemetry_record_t record;
            memcpy(&record,
                   sector_data + TELEMETRY_SECTOR_HEADER_SIZE +
                       slot * TELEMETRY_RECORD_SIZE,
                   sizeof(record));
            if (bytes_are_erased(&record, sizeof(record))) continue;
            if (!telemetry_record_valid(&record)) continue;
            if (record.sample.timestamp < start_timestamp ||
                record.sample.timestamp >= end_timestamp) {
                continue;
            }
            if (*count < capacity) {
                samples[(*count)++] = record.sample;
            } else {
                size_t oldest = 0;
                for (size_t i = 1; i < *count; i++) {
                    if (telemetry_sample_compare(&samples[i], &samples[oldest]) < 0) {
                        oldest = i;
                    }
                }
                if (telemetry_sample_compare(&record.sample, &samples[oldest]) > 0) {
                    samples[oldest] = record.sample;
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
        memset(&record, 0xFF, sizeof(record));
        record.magic = TELEMETRY_RECORD_MAGIC;
        record.sequence = s_telemetry_next_sequence;
        record.sample = samples[i];
        record.sample.sequence = s_telemetry_next_sequence;
        record.reserved = 0xFFFFFFFF;
        record.crc32 = telemetry_record_crc(&record);
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
