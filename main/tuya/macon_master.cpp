/*
 * Active Tuya master runtime. See macon_master.h.
 */
#include "macon_master.h"

#include <atomic>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "macon_uart_transport.h"
#include "tuya_codec.h"
#include "macon_link.h"
#include "heatpump_controller.h"   // feedRegisterWindow / recordObservedWindow

static const char *TAG = "macon_master";

namespace macon_master {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static constexpr int    POLL_INTERVAL_MS    = 800;   // gap between poll cycles
static constexpr int    RESP_TIMEOUT_MS     = 200;   // per transport read slice
static constexpr int    POLL_TXN_DEADLINE_MS = 500;  // whole read of one window
static constexpr int    PREFLIGHT_MS        = 2000;  // bus-idle listen window
static constexpr size_t ACC_CAP             = 192;   // >= echo(9)+resp(67)+tag+slack

// The two windows the OEM controller rotates through: telemetry (reg2093..) and
// holding (reg2000..). Indices into tuya_codec::KNOWN_WINDOWS.
static const tuya_codec::RegWindow &TELEMETRY_WIN = tuya_codec::KNOWN_WINDOWS[0]; // {0,50,2093}
static const tuya_codec::RegWindow &HOLDING_WIN   = tuya_codec::KNOWN_WINDOWS[1]; // {50,58,2000}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static tuya::MaconUartTransport s_transport;   // trivial ctor; hardware set up in init()
static arctic::MaconLink       *s_link       = nullptr;
static SemaphoreHandle_t        s_bus_mutex   = nullptr;
static TaskHandle_t             s_task        = nullptr;
static std::atomic<bool>        s_active{false};
static bool                     s_initialized = false;

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// One request/response transaction: send an fc=0x03 read of `win`, accumulate
// the reply, and feed the decoded register bytes into HeatPumpState. Tolerates
// our own echoed request frame, unrelated frames, junk, and the trailing
// block-tag byte (all skipped by find_frame_start / parse_frame). Must be
// called with s_bus_mutex held. Returns true if the matching response was fed.
// ---------------------------------------------------------------------------
static bool poll_window(const tuya_codec::RegWindow &win)
{
    uint8_t req[16];
    const size_t n = tuya_codec::encode_request(req, sizeof(req),
                                                tuya_codec::FC_READ,
                                                win.field_a, win.field_b);
    if (n == 0) return false;

    // Bus is idle here (we hold the mutex and nothing else transmits): drop any
    // leftover trailing-tag / late bytes from the previous transaction so they
    // cannot desync this one.
    s_transport.flush_rx();

    if (s_transport.write(req, n) < static_cast<int>(n)) {
        ESP_LOGW(TAG, "poll write failed (addr=%u)", (unsigned)win.field_a);
        return false;
    }

    uint8_t acc[ACC_CAP];
    size_t  len      = 0;
    const uint32_t deadline = now_ms() + POLL_TXN_DEADLINE_MS;

    while ((int32_t)(deadline - now_ms()) > 0) {
        // Drain complete frames at the head of the accumulator.
        while (len >= tuya_codec::HDR_LEN) {
            const size_t start = tuya_codec::find_frame_start(acc, len);
            if (start == len) {
                // No plausible frame start: keep only a possible partial magic.
                const size_t keep = (len < tuya_codec::HDR_LEN - 1)
                                        ? len : tuya_codec::HDR_LEN - 1;
                std::memmove(acc, acc + (len - keep), keep);
                len = keep;
                break;
            }
            if (start > 0) {
                std::memmove(acc, acc + start, len - start);
                len -= start;
                continue;
            }

            tuya_codec::ParsedFrame pf;
            const tuya_codec::ParseResult r =
                tuya_codec::parse_frame(acc, len, pf);
            if (r == tuya_codec::ParseResult::TRUNCATED) {
                break;  // need more bytes
            }
            if (r == tuya_codec::ParseResult::OK) {
                const bool match = pf.dir == tuya_codec::DIR_RESPONSE &&
                                   pf.fc  == tuya_codec::FC_READ &&
                                   pf.field_a == win.field_a &&
                                   pf.field_b == win.field_b;
                if (match) {
                    if (pf.window && pf.payload &&
                        pf.payload_len > pf.window->prefix_len) {
                        arctic::feedRegisterWindow(
                            pf.window->reg_base,
                            pf.payload + pf.window->prefix_len,
                            pf.payload_len - pf.window->prefix_len);
                    }
                    arctic::recordObservedWindow(pf.field_a, pf.field_b, 1,
                                                 pf.payload, pf.payload_len);
                    return true;
                }
                // Valid but unrelated (e.g. our echoed request) -> skip it.
                std::memmove(acc, acc + pf.frame_len, len - pf.frame_len);
                len -= pf.frame_len;
                continue;
            }
            // Bad frame at head (checksum/etc.) -> drop one byte and resync.
            std::memmove(acc, acc + 1, len - 1);
            len -= 1;
        }

        if (len >= ACC_CAP) {  // overflow guard: garbage stream
            len = 0;
        }
        const int remaining = (int)(deadline - now_ms());
        if (remaining <= 0) break;
        const int got = s_transport.read(acc + len, ACC_CAP - len,
                                         remaining < RESP_TIMEOUT_MS
                                             ? remaining : RESP_TIMEOUT_MS);
        if (got < 0) {
            ESP_LOGW(TAG, "poll read error (addr=%u)", (unsigned)win.field_a);
            return false;
        }
        len += (size_t)got;   // got==0 just means this slice timed out; loop re-checks deadline
    }

    ESP_LOGW(TAG, "poll timeout: no response for addr=%u count=%u",
             (unsigned)win.field_a, (unsigned)win.field_b);
    return false;
}

// ---------------------------------------------------------------------------
// Preflight: listen for PREFLIGHT_MS. If ANY valid Tuya frame is seen, another
// master (the OEM controller) is driving the bus, so it is NOT safe for us to
// transmit. Returns true only if the bus stayed quiet.
// ---------------------------------------------------------------------------
static bool preflight_bus_idle()
{
    uint8_t acc[ACC_CAP];
    size_t  len = 0;
    const uint32_t deadline = now_ms() + PREFLIGHT_MS;

    while ((int32_t)(deadline - now_ms()) > 0) {
        const int got = s_transport.read(acc + len, ACC_CAP - len, 100);
        if (got > 0) {
            len += (size_t)got;
            // Scan for any valid frame.
            while (len >= tuya_codec::HDR_LEN) {
                const size_t start = tuya_codec::find_frame_start(acc, len);
                if (start == len) {
                    const size_t keep = (len < tuya_codec::HDR_LEN - 1)
                                            ? len : tuya_codec::HDR_LEN - 1;
                    std::memmove(acc, acc + (len - keep), keep);
                    len = keep;
                    break;
                }
                if (start > 0) {
                    std::memmove(acc, acc + start, len - start);
                    len -= start;
                    continue;
                }
                tuya_codec::ParsedFrame pf;
                const tuya_codec::ParseResult r =
                    tuya_codec::parse_frame(acc, len, pf);
                if (r == tuya_codec::ParseResult::TRUNCATED) break;
                if (r == tuya_codec::ParseResult::OK) {
                    ESP_LOGE(TAG,
                             "Preflight: live bus traffic detected (dir=0x%02X addr=%u) "
                             "- another master present. Refusing to activate.",
                             pf.dir, (unsigned)pf.field_a);
                    return false;
                }
                std::memmove(acc, acc + 1, len - 1);
                len -= 1;
            }
            if (len >= ACC_CAP) len = 0;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Poll task
// ---------------------------------------------------------------------------
static void poll_task(void *)
{
    ESP_LOGI(TAG, "Active-master poll task started");
    for (;;) {
        if (s_bus_mutex) {
            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            poll_window(HOLDING_WIN);     // regs 2000.. (electrical/status/mode)
            xSemaphoreGive(s_bus_mutex);

            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            poll_window(TELEMETRY_WIN);   // regs 2093.. (setpoint/temps/EEV)
            xSemaphoreGive(s_bus_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t init()
{
    if (s_initialized) return ESP_OK;

    const esp_err_t err = s_transport.init();
    if (err != ESP_OK) return err;

    if (s_bus_mutex == nullptr) {
        s_bus_mutex = xSemaphoreCreateMutex();
        if (s_bus_mutex == nullptr) {
            ESP_LOGE(TAG, "failed to create bus mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_link == nullptr) {
        s_link = new arctic::MaconLink(s_transport, RESP_TIMEOUT_MS);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "macon_master initialised");
    return ESP_OK;
}

esp_err_t start()
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "start() before init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active.load()) return ESP_OK;

    ESP_LOGI(TAG, "Preflight: listening %d ms for other bus masters...", PREFLIGHT_MS);
    if (!preflight_bus_idle()) {
        ESP_LOGE(TAG,
                 "Bus is NOT idle - the OEM controller appears to still be "
                 "connected. Staying passive; no telemetry or setpoint writes.");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Preflight passed (bus quiet). Becoming active master.");

    // Mark active before the task starts so setters queued immediately after
    // start() are honoured; clear it again if task creation fails.
    s_active.store(true);
    if (xTaskCreate(poll_task, "macon_master", 4096, nullptr, 5, &s_task) != pdPASS) {
        s_active.store(false);
        s_task = nullptr;
        ESP_LOGE(TAG, "failed to create poll task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool is_active()
{
    return s_active.load();
}

static bool write_setpoint(int celsius, bool cooling)
{
    if (!s_active.load() || s_link == nullptr || s_bus_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    s_transport.flush_rx();  // bus idle under the mutex: drop any stale bytes
    const arctic::MaconResult r = cooling
        ? s_link->set_cooling_setpoint(celsius)
        : s_link->set_hot_water_setpoint(celsius);
    if (r != arctic::MaconResult::Ok) {
        s_transport.flush_rx();  // drain a possible late/partial reply to idle
    }
    xSemaphoreGive(s_bus_mutex);

    if (r == arctic::MaconResult::Ok) {
        ESP_LOGI(TAG, "%s setpoint -> %d C (ACKed)",
                 cooling ? "cooling" : "hot-water", celsius);
        return true;
    }
    ESP_LOGW(TAG, "%s setpoint write failed: %s",
             cooling ? "cooling" : "hot-water", arctic::macon_result_name(r));
    return false;
}

bool set_cooling_setpoint(int celsius)   { return write_setpoint(celsius, true);  }
bool set_hot_water_setpoint(int celsius) { return write_setpoint(celsius, false); }

bool write_register(uint16_t address, uint8_t value)
{
    if (!s_active.load() || s_link == nullptr || s_bus_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    s_transport.flush_rx();
    const arctic::MaconResult r = s_link->write_register(address, value);
    if (r != arctic::MaconResult::Ok) {
        s_transport.flush_rx();
    }
    xSemaphoreGive(s_bus_mutex);

    if (r == arctic::MaconResult::Ok) {
        ESP_LOGI(TAG, "register %u -> %u (ACKed)",
                 (unsigned)address, (unsigned)value);
        return true;
    }
    ESP_LOGW(TAG, "register %u write failed: %s",
             (unsigned)address, arctic::macon_result_name(r));
    return false;
}

}  // namespace macon_master
