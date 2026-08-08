#include "history_storage.h"

#include <esp_crc.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <stddef.h>
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

static_assert(sizeof(event_bank_header_t) == 32, "Unexpected event bank header size");
static_assert(sizeof(event_journal_record_t) == EVENT_RECORD_SIZE,
              "Event journal records must remain fixed-size");

static const esp_partition_t* s_partition = nullptr;
static int s_active_bank = -1;
static uint32_t s_generation = 0;
static uint32_t s_next_journal_sequence = 1;
static size_t s_next_record = 0;

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
