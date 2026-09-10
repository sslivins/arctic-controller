/*
 * Host implementation of the SNTP stub. See esp_sntp.h.
 */

#include "esp_sntp.h"

#include <map>
#include <string>

namespace {
sntp_sync_time_cb_t g_cb = nullptr;
bool g_enabled = false;
uint32_t g_interval = 0;
sntp_operatingmode_t g_mode = SNTP_OPMODE_POLL;
std::map<uint8_t, std::string> g_servers;
int g_init_count = 0;
int g_stop_count = 0;
int g_mode_count = 0;
int g_cb_count = 0;
}  // namespace

extern "C" {

void esp_sntp_setoperatingmode(sntp_operatingmode_t mode) {
    g_mode = mode;
    ++g_mode_count;
}

void esp_sntp_setservername(uint8_t idx, const char *server) {
    g_servers[idx] = server ? server : "";
}

const char *esp_sntp_getservername(uint8_t idx) {
    auto it = g_servers.find(idx);
    return it == g_servers.end() ? nullptr : it->second.c_str();
}

void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb) {
    g_cb = cb;
    ++g_cb_count;
}

void esp_sntp_set_sync_interval(uint32_t ms) { g_interval = ms; }

uint32_t esp_sntp_get_sync_interval(void) { return g_interval; }

void esp_sntp_init(void) {
    g_enabled = true;
    ++g_init_count;
}

void esp_sntp_stop(void) {
    g_enabled = false;
    ++g_stop_count;
}

bool esp_sntp_enabled(void) { return g_enabled; }

}  // extern "C"

namespace sntp_fake {

void reset() {
    g_cb = nullptr;
    g_enabled = false;
    g_interval = 0;
    g_mode = SNTP_OPMODE_POLL;
    g_servers.clear();
    g_init_count = 0;
    g_stop_count = 0;
    g_mode_count = 0;
    g_cb_count = 0;
}

void deliver_sync(time_t epoch_seconds) {
    if (!g_cb) return;
    struct timeval tv;
    tv.tv_sec = epoch_seconds;
    tv.tv_usec = 0;
    g_cb(&tv);
}

void deliver_sync_null() {
    if (!g_cb) return;
    g_cb(nullptr);
}

int init_count() { return g_init_count; }
int stop_count() { return g_stop_count; }
int operating_mode_set_count() { return g_mode_count; }
int callback_set_count() { return g_cb_count; }
bool has_callback() { return g_cb != nullptr; }

std::string server(uint8_t idx) {
    auto it = g_servers.find(idx);
    return it == g_servers.end() ? std::string() : it->second;
}

uint32_t sync_interval() { return g_interval; }
sntp_operatingmode_t operating_mode() { return g_mode; }

}  // namespace sntp_fake
