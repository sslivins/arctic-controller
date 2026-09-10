/*
 * Host stub for esp_sntp.h.
 *
 * The point of this fake is that it can DELIVER a sync callback. #217 notes
 * that the existing time-sync test "proves command acceptance, not that NTP
 * succeeded or the clock moved". On the device you cannot make an NTP server
 * reply with a chosen timestamp on demand; here you can, including the reply
 * that must be refused (a server claiming 1970, which would otherwise latch
 * the device into believing its clock is valid).
 *
 * Configuration is recorded rather than acted on, so a test can assert the
 * servers and interval that were actually programmed instead of trusting that
 * configure_sntp() was reached.
 */
#pragma once

#include <stdint.h>
#include <sys/time.h>

typedef enum {
    SNTP_OPMODE_POLL = 0,
    SNTP_OPMODE_LISTENONLY = 1,
} sntp_operatingmode_t;

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);

#ifdef __cplusplus
extern "C" {
#endif

void esp_sntp_setoperatingmode(sntp_operatingmode_t mode);
void esp_sntp_setservername(uint8_t idx, const char *server);
const char *esp_sntp_getservername(uint8_t idx);
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb);
void esp_sntp_set_sync_interval(uint32_t ms);
uint32_t esp_sntp_get_sync_interval(void);
void esp_sntp_init(void);
void esp_sntp_stop(void);
bool esp_sntp_enabled(void);

#ifdef __cplusplus
}

#include <string>

namespace sntp_fake {

// Drop all configuration, counters and the registered callback.
void reset();

// Deliver a sync notification with the given epoch seconds, exactly as the
// SNTP task does when a server replies. Does nothing if no callback has been
// registered -- which is itself worth asserting.
void deliver_sync(time_t epoch_seconds);

// Deliver a notification with a NULL timeval, which the IDF does not normally
// do but which the production callback explicitly guards against.
void deliver_sync_null();

int init_count();
int stop_count();
int operating_mode_set_count();
int callback_set_count();
bool has_callback();
std::string server(uint8_t idx);
uint32_t sync_interval();
sntp_operatingmode_t operating_mode();

}  // namespace sntp_fake
#endif
