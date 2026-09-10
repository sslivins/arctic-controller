/*
 * RAM-backed implementation of the flash fake. See esp_partition_fake.h.
 */

#include "esp_partition_fake.h"

#include <sys/mman.h>

#include <cstring>
#include <cstdio>
#include <map>

namespace {

constexpr size_t SECTOR = 4096;
constexpr size_t ARENA_DATA_SIZE = 8u * 1024 * 1024;
constexpr size_t MAX_SLOTS = 4;

// The backing store lives in shared anonymous memory, not the heap.
//
// Tests run each phase in a forked child so that history_storage.cpp's
// file-scope statics start cold (its init() early-returns once s_partition is
// set, so an in-process "reboot" is a no-op and every persistence assertion
// would pass vacuously). A reboot is only meaningful if the medium outlives
// the process, so the image has to be shared memory: fresh RAM, same flash.
struct SharedSlot {
    char label[17];  // matches esp_partition_t::label
    uint8_t subtype;
    uint32_t size;
    uint32_t data_off;
};

struct SharedArena {
    uint32_t slot_count;
    uint32_t bump;
    SharedSlot slots[MAX_SLOTS];
    uint8_t data[ARENA_DATA_SIZE];
};

SharedArena *g_arena = nullptr;

SharedArena &arena() {
    if (g_arena == nullptr) {
        void *p = mmap(nullptr, sizeof(SharedArena), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "flash_fake: mmap failed\n");
            std::abort();
        }
        g_arena = static_cast<SharedArena *>(p);
        g_arena->slot_count = 0;
        g_arena->bump = 0;
    }
    return *g_arena;
}

SharedSlot *slot_for(const char *label) {
    SharedArena &a = arena();
    for (uint32_t i = 0; i < a.slot_count; ++i) {
        if (std::strcmp(a.slots[i].label, label) == 0) {
            return &a.slots[i];
        }
    }
    return nullptr;
}

uint8_t *image_of(SharedSlot *s) { return arena().data + s->data_off; }

// Descriptors are process-local so the pointers handed to production code stay
// valid for the life of the process.
std::map<std::string, esp_partition_t> g_descs;

const esp_partition_t *desc_for(SharedSlot *s) {
    auto it = g_descs.find(s->label);
    if (it == g_descs.end()) {
        esp_partition_t d;
        std::memset(&d, 0, sizeof(d));
        d.type = ESP_PARTITION_TYPE_DATA;
        d.subtype = static_cast<esp_partition_subtype_t>(s->subtype);
        d.address = 0;
        d.size = s->size;
        d.erase_size = SECTOR;
        std::snprintf(d.label, sizeof(d.label), "%s", s->label);
        d.encrypted = false;
        it = g_descs.emplace(s->label, d).first;
    }
    return &it->second;
}

struct Injection {
    esp_err_t err;
    int remaining;
    int skip;  // let this many calls through first
};
std::map<int, Injection> g_injections;
std::map<int, int> g_calls;

int g_power_loss_countdown = -1;  // <0 == powered
bool g_powered = true;
size_t g_bytes_written = 0;
size_t g_bytes_erased = 0;

int key_of(flash_fake::Op op) { return static_cast<int>(op); }

esp_err_t take_injection(flash_fake::Op op) {
    g_calls[key_of(op)] += 1;
    auto it = g_injections.find(key_of(op));
    if (it == g_injections.end()) {
        return ESP_OK;
    }
    esp_err_t err = it->second.err;
    if (it->second.skip > 0) {
        --it->second.skip;
        return ESP_OK;
    }
    if (it->second.remaining > 0 && --it->second.remaining == 0) {
        g_injections.erase(it);
    }
    return err;
}

// Count a completed medium-touching operation and decide whether the device
// still has power for the next one.
void tick_power() {
    if (g_power_loss_countdown < 0) {
        return;
    }
    if (g_power_loss_countdown == 0) {
        g_powered = false;
        return;
    }
    --g_power_loss_countdown;
    if (g_power_loss_countdown == 0) {
        g_powered = false;
    }
}

struct Located {
    SharedSlot *slot;
    uint8_t *image;
    size_t size;
};

bool locate(const esp_partition_t *p, Located *out) {
    if (!p) {
        return false;
    }
    SharedSlot *s = slot_for(p->label);
    if (!s) {
        return false;
    }
    out->slot = s;
    out->image = image_of(s);
    out->size = s->size;
    return true;
}

}  // namespace

extern "C" {

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label) {
    SharedArena &a = arena();
    for (uint32_t i = 0; i < a.slot_count; ++i) {
        SharedSlot *s = &a.slots[i];
        if (type != ESP_PARTITION_TYPE_ANY && type != ESP_PARTITION_TYPE_DATA) {
            continue;
        }
        if (subtype != ESP_PARTITION_SUBTYPE_ANY && s->subtype != subtype) {
            continue;
        }
        if (label != nullptr && std::strcmp(label, s->label) != 0) {
            continue;
        }
        return desc_for(s);
    }
    return nullptr;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset,
                             void *dst, size_t size) {
    esp_err_t injected = take_injection(flash_fake::Op::Read);
    if (injected != ESP_OK) {
        return injected;
    }
    Located loc{};
    if (!locate(partition, &loc) || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_offset + size > loc.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(dst, loc.image + src_offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset,
                              const void *src, size_t size) {
    esp_err_t injected = take_injection(flash_fake::Op::Write);
    if (injected != ESP_OK) {
        return injected;
    }
    Located loc{};
    if (!locate(partition, &loc) || !src) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dst_offset + size > loc.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!g_powered) {
        // Power is gone: the call "succeeds" from the caller's point of view
        // but nothing reaches the medium. This is the interesting case.
        return ESP_OK;
    }
    const uint8_t *in = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i < size; ++i) {
        // NOR: programming can only clear bits.
        loc.image[dst_offset + i] &= in[i];
    }
    g_bytes_written += size;
    tick_power();
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset,
                                    size_t size) {
    esp_err_t injected = take_injection(flash_fake::Op::Erase);
    if (injected != ESP_OK) {
        return injected;
    }
    Located loc{};
    if (!locate(partition, &loc)) {
        return ESP_ERR_INVALID_ARG;
    }
    // The real driver rejects unaligned erases; a fake that accepted them
    // would hide a class of bug that only appears on hardware.
    if ((offset % SECTOR) != 0 || (size % SECTOR) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset + size > loc.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!g_powered) {
        return ESP_OK;
    }
    std::memset(loc.image + offset, 0xff, size);
    g_bytes_erased += size;
    tick_power();
    return ESP_OK;
}

}  // extern "C"

namespace flash_fake {

void install(const std::string &label, uint8_t subtype, uint32_t size) {
    SharedArena &a = arena();
    SharedSlot *s = slot_for(label.c_str());
    if (s == nullptr) {
        if (a.slot_count >= MAX_SLOTS || a.bump + size > ARENA_DATA_SIZE) {
            std::fprintf(stderr, "flash_fake: arena exhausted\n");
            std::abort();
        }
        s = &a.slots[a.slot_count++];
        std::snprintf(s->label, sizeof(s->label), "%s", label.c_str());
        s->data_off = a.bump;
        a.bump += size;
    }
    s->subtype = subtype;
    s->size = size;
    std::memset(image_of(s), 0xff, size);
    g_descs.erase(label);
}

void install_history_partition() {
    // Matches partitions.csv: history,data,0x40,0x910000,2M
    install("history", 0x40, 2 * 1024 * 1024);
}

void reset() {
    SharedArena &a = arena();
    a.slot_count = 0;
    a.bump = 0;
    g_descs.clear();
    g_injections.clear();
    g_calls.clear();
    g_power_loss_countdown = -1;
    g_powered = true;
    g_bytes_written = 0;
    g_bytes_erased = 0;
}

void power_loss_after(int n) {
    g_power_loss_countdown = n;
    g_powered = (n != 0);
}

void fail_next(Op op, esp_err_t err, int count) {
    g_injections[key_of(op)] = Injection{err, count, 0};
}

void fail_nth(Op op, esp_err_t err, int nth) {
    g_injections[key_of(op)] = Injection{err, 1, nth > 0 ? nth - 1 : 0};
}

void clear_failures() { g_injections.clear(); }

int call_count(Op op) {
    auto it = g_calls.find(key_of(op));
    return it == g_calls.end() ? 0 : it->second;
}

size_t bytes_written() { return g_bytes_written; }
size_t bytes_erased() { return g_bytes_erased; }

bool peek(const std::string &label, size_t offset, void *dst, size_t size) {
    SharedSlot *s = slot_for(label.c_str());
    if (!s || offset + size > s->size) {
        return false;
    }
    std::memcpy(dst, image_of(s) + offset, size);
    return true;
}

void poke(const std::string &label, size_t offset, const void *src, size_t size) {
    SharedSlot *s = slot_for(label.c_str());
    if (!s || offset + size > s->size) {
        return;
    }
    std::memcpy(image_of(s) + offset, src, size);
}

void corrupt_byte(const std::string &label, size_t offset, uint8_t and_mask) {
    SharedSlot *s = slot_for(label.c_str());
    if (!s || offset >= s->size) {
        return;
    }
    image_of(s)[offset] &= and_mask;
}

bool is_erased(const std::string &label, size_t offset, size_t size) {
    SharedSlot *s = slot_for(label.c_str());
    if (!s || offset + size > s->size) {
        return false;
    }
    const uint8_t *img = image_of(s);
    for (size_t i = 0; i < size; ++i) {
        if (img[offset + i] != 0xff) {
            return false;
        }
    }
    return true;
}

}  // namespace flash_fake
