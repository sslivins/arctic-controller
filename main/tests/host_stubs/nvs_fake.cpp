/*
 * In-memory implementation of the NVS host stub. See nvs_fake.h for rationale.
 *
 * Model: each namespace holds a map of committed values. A handle opened
 * READWRITE accumulates pending writes that become committed only on
 * nvs_commit(), which is what makes "power lost before commit" expressible.
 * Reads observe pending writes through the same handle, matching IDF.
 */

#include "nvs_fake.h"
#include "nvs_flash.h"

#include <cstring>
#include <map>
#include <vector>

namespace {

using Blob = std::vector<uint8_t>;

struct Entry {
    enum class Kind { U8, U32, Str, Blob } kind;
    uint8_t u8 = 0;
    uint32_t u32 = 0;
    std::string str;
    std::vector<uint8_t> blob;
};

using Namespace = std::map<std::string, Entry>;

struct Handle {
    bool open = false;
    std::string ns;
    bool writable = false;
    Namespace pending;
};

std::map<std::string, Namespace> g_committed;
std::map<nvs_handle_t, Handle> g_handles;
nvs_handle_t g_next_handle = 1;

struct Injection {
    esp_err_t err;
    int remaining;
};
std::map<int, Injection> g_injections;
std::map<int, int> g_calls;

int key_of(nvs_fake::Op op) { return static_cast<int>(op); }

// Returns the injected error for `op` if one is pending, else ESP_OK.
esp_err_t take_injection(nvs_fake::Op op) {
    g_calls[key_of(op)] += 1;
    auto it = g_injections.find(key_of(op));
    if (it == g_injections.end()) {
        return ESP_OK;
    }
    esp_err_t err = it->second.err;
    if (it->second.remaining > 0 && --it->second.remaining == 0) {
        g_injections.erase(it);
    }
    return err;
}

Handle *lookup(nvs_handle_t handle) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end() || !it->second.open) {
        return nullptr;
    }
    return &it->second;
}

// Resolve a key through pending-then-committed, as a real handle would see it.
const Entry *find(Handle *h, const char *key) {
    auto p = h->pending.find(key);
    if (p != h->pending.end()) {
        return &p->second;
    }
    auto ns = g_committed.find(h->ns);
    if (ns == g_committed.end()) {
        return nullptr;
    }
    auto e = ns->second.find(key);
    return e == ns->second.end() ? nullptr : &e->second;
}

esp_err_t begin_write(nvs_handle_t handle, nvs_fake::Op op, Handle **out) {
    esp_err_t injected = take_injection(op);
    if (injected != ESP_OK) {
        return injected;
    }
    Handle *h = lookup(handle);
    if (!h) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!h->writable) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    *out = h;
    return ESP_OK;
}

esp_err_t begin_read(nvs_handle_t handle, nvs_fake::Op op, Handle **out) {
    esp_err_t injected = take_injection(op);
    if (injected != ESP_OK) {
        return injected;
    }
    Handle *h = lookup(handle);
    if (!h) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    *out = h;
    return ESP_OK;
}

}  // namespace

extern "C" {

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    esp_err_t injected = take_injection(nvs_fake::Op::Open);
    if (injected != ESP_OK) {
        return injected;
    }
    if (!name || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    // IDF returns NOT_FOUND when a namespace that has never been written is
    // opened read-only. Code that treats that as fatal rather than "use the
    // default" would be wrong on a factory-fresh device, so the fake models it.
    if (open_mode == NVS_READONLY && g_committed.find(name) == g_committed.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_handle_t handle = g_next_handle++;
    Handle h;
    h.open = true;
    h.ns = name;
    h.writable = (open_mode == NVS_READWRITE);
    g_handles[handle] = h;
    *out_handle = handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    auto it = g_handles.find(handle);
    if (it != g_handles.end()) {
        it->second.open = false;
        it->second.pending.clear();
    }
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    esp_err_t injected = take_injection(nvs_fake::Op::Commit);
    if (injected != ESP_OK) {
        return injected;
    }
    Handle *h = lookup(handle);
    if (!h) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    for (const auto &kv : h->pending) {
        g_committed[h->ns][kv.first] = kv.second;
    }
    h->pending.clear();
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value) {
    Handle *h = nullptr;
    esp_err_t err = begin_read(handle, nvs_fake::Op::GetU8, &h);
    if (err != ESP_OK) {
        return err;
    }
    const Entry *e = find(h, key);
    if (!e || e->kind != Entry::Kind::U8) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value) {
        *out_value = e->u8;
    }
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::SetU8, &h);
    if (err != ESP_OK) {
        return err;
    }
    Entry e;
    e.kind = Entry::Kind::U8;
    e.u8 = value;
    h->pending[key] = e;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value) {
    Handle *h = nullptr;
    esp_err_t err = begin_read(handle, nvs_fake::Op::GetU32, &h);
    if (err != ESP_OK) {
        return err;
    }
    const Entry *e = find(h, key);
    if (!e || e->kind != Entry::Kind::U32) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value) {
        *out_value = e->u32;
    }
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::SetU32, &h);
    if (err != ESP_OK) {
        return err;
    }
    Entry e;
    e.kind = Entry::Kind::U32;
    e.u32 = value;
    h->pending[key] = e;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length) {
    Handle *h = nullptr;
    esp_err_t err = begin_read(handle, nvs_fake::Op::GetStr, &h);
    if (err != ESP_OK) {
        return err;
    }
    const Entry *e = find(h, key);
    if (!e || e->kind != Entry::Kind::Str) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!length) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t needed = e->str.size() + 1;
    // IDF's size-query convention: out_value == NULL asks for the length only.
    if (out_value == nullptr) {
        *length = needed;
        return ESP_OK;
    }
    if (*length < needed) {
        *length = needed;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::memcpy(out_value, e->str.c_str(), needed);
    *length = needed;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::SetStr, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    Entry e;
    e.kind = Entry::Kind::Str;
    e.str = value;
    h->pending[key] = e;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length) {
    Handle *h = nullptr;
    esp_err_t err = begin_read(handle, nvs_fake::Op::GetBlob, &h);
    if (err != ESP_OK) {
        return err;
    }
    const Entry *e = find(h, key);
    if (!e || e->kind != Entry::Kind::Blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!length) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t needed = e->blob.size();
    // IDF's size-query convention: out_value == NULL asks for the length only.
    if (out_value == nullptr) {
        *length = needed;
        return ESP_OK;
    }
    if (*length < needed) {
        *length = needed;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::memcpy(out_value, e->blob.data(), needed);
    *length = needed;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::SetBlob, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (!value && length > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    Entry e;
    e.kind = Entry::Kind::Blob;
    const uint8_t *src = static_cast<const uint8_t *>(value);
    e.blob.assign(src, src + length);
    h->pending[key] = e;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::EraseKey, &h);
    if (err != ESP_OK) {
        return err;
    }
    h->pending.erase(key);
    auto ns = g_committed.find(h->ns);
    if (ns != g_committed.end()) {
        ns->second.erase(key);
    }
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
    Handle *h = nullptr;
    esp_err_t err = begin_write(handle, nvs_fake::Op::EraseKey, &h);
    if (err != ESP_OK) {
        return err;
    }
    h->pending.clear();
    g_committed.erase(h->ns);
    return ESP_OK;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }

esp_err_t nvs_flash_erase(void) {
    g_committed.clear();
    return ESP_OK;
}

esp_err_t nvs_flash_deinit(void) { return ESP_OK; }

}  // extern "C"

namespace nvs_fake {

void reset() {
    g_committed.clear();
    g_handles.clear();
    g_next_handle = 1;
    g_injections.clear();
    g_calls.clear();
}

void reset_volatile() {
    for (auto &kv : g_handles) {
        kv.second.pending.clear();
    }
}

void fail_next(Op op, esp_err_t err, int count) {
    g_injections[key_of(op)] = Injection{err, count};
}

void clear_failures() { g_injections.clear(); }

int call_count(Op op) {
    auto it = g_calls.find(key_of(op));
    return it == g_calls.end() ? 0 : it->second;
}

bool peek_u8(const std::string &ns, const std::string &key, uint8_t *out) {
    auto n = g_committed.find(ns);
    if (n == g_committed.end()) {
        return false;
    }
    auto e = n->second.find(key);
    if (e == n->second.end() || e->second.kind != Entry::Kind::U8) {
        return false;
    }
    if (out) {
        *out = e->second.u8;
    }
    return true;
}

bool peek_blob(const std::string &ns, const std::string &key,
               std::vector<uint8_t> *out) {
    auto n = g_committed.find(ns);
    if (n == g_committed.end()) {
        return false;
    }
    auto e = n->second.find(key);
    if (e == n->second.end() || e->second.kind != Entry::Kind::Blob) {
        return false;
    }
    if (out) {
        *out = e->second.blob;
    }
    return true;
}

bool peek_str(const std::string &ns, const std::string &key, std::string *out) {
    auto n = g_committed.find(ns);
    if (n == g_committed.end()) {
        return false;
    }
    auto e = n->second.find(key);
    if (e == n->second.end() || e->second.kind != Entry::Kind::Str) {
        return false;
    }
    if (out) {
        *out = e->second.str;
    }
    return true;
}

void seed_u8(const std::string &ns, const std::string &key, uint8_t value) {
    Entry e;
    e.kind = Entry::Kind::U8;
    e.u8 = value;
    g_committed[ns][key] = e;
}

void seed_str(const std::string &ns, const std::string &key, const std::string &value) {
    Entry e;
    e.kind = Entry::Kind::Str;
    e.str = value;
    g_committed[ns][key] = e;
}

void seed_blob(const std::string &ns, const std::string &key,
               const std::vector<uint8_t> &value) {
    Entry e;
    e.kind = Entry::Kind::Blob;
    e.blob = value;
    g_committed[ns][key] = e;
}

}  // namespace nvs_fake
