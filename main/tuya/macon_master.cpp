/*
 * Active Tuya master runtime — ESP-IDF shim. See macon_master_iface.h.
 *
 * The wire logic (window polling, bus-idle preflight, verified writes) now
 * lives in the shared library as arctic::MaconMaster. This file is only the
 * platform glue the library deliberately does not own:
 *
 *   * the concrete RS485 UART transport (tuya::MaconUartTransport),
 *   * a monotonic clock over esp_timer (EspClock),
 *   * the FreeRTOS poll task and the bus mutex that serialises transactions.
 *
 * Decoded windows are ingested into the opaque register image by the library
 * sink returned from arctic::liveIngestSink(); a poll cycle is bracketed by
 * arctic::liveIngestBeginCycle()/liveIngestEndCycle(). No register/bit/frame
 * detail ever reaches this shim — it is all behind the library API.
 */
#include "macon_master_iface.h"    // this module's public API (macon_master::)

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "macon_uart_transport.h"   // tuya::MaconUartTransport
#include "macon_master.h"           // arctic::MaconMaster / MaconClock
#include "macon_link.h"             // arctic::MaconResult / macon_result_name
#include "heatpump_controller.h"    // arctic::liveIngestSink / liveIngestBeginCycle / liveIngestEndCycle

static const char *TAG = "macon_master";

namespace macon_master {

// ---------------------------------------------------------------------------
// Tunables (controller-side cadence + timeouts handed to the library master)
// ---------------------------------------------------------------------------
static constexpr int POLL_INTERVAL_MS     = 800;   // gap between poll cycles
static constexpr int RESP_TIMEOUT_MS      = 200;   // per transport read slice
static constexpr int POLL_TXN_DEADLINE_MS = 500;   // whole read of one window
static constexpr int PREFLIGHT_MS         = 2000;  // bus-idle listen window

// ---------------------------------------------------------------------------
// Platform adapters injected into arctic::MaconMaster
// ---------------------------------------------------------------------------

// Monotonic millisecond clock over esp_timer.
class EspClock : public arctic::MaconClock {
public:
    uint32_t now_ms() override {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static tuya::MaconUartTransport s_transport;   // trivial ctor; hardware set up in init()
static EspClock                 s_clock;
static arctic::MaconMaster     *s_master      = nullptr;
static SemaphoreHandle_t        s_bus_mutex   = nullptr;
static TaskHandle_t             s_task        = nullptr;
static std::atomic<bool>        s_active{false};
static bool                     s_initialized = false;

// ---------------------------------------------------------------------------
// Poll task — serialises each window read through the bus mutex so the
// half-duplex UART is only ever driven by one transaction at a time. The mutex
// is held per transaction, not per cycle, so a UI/REST setpoint write waits at
// most one in-flight transaction.
// ---------------------------------------------------------------------------
static void poll_task(void *)
{
    ESP_LOGI(TAG, "Active-master poll task started");
    for (;;) {
        if (s_bus_mutex && s_master) {
            // Each poll cycle is bracketed by the controller's image-lock so the
            // library sink can ingest decoded windows internally; the bus mutex
            // is released between polls so a UI/REST write waits at most one
            // in-flight transaction.
            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            arctic::liveIngestBeginCycle();
            s_master->poll_holding();
            arctic::liveIngestEndCycle();
            xSemaphoreGive(s_bus_mutex);

            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            arctic::liveIngestBeginCycle();
            s_master->poll_telemetry();
            arctic::liveIngestEndCycle();
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
    if (s_master == nullptr) {
        s_master = new arctic::MaconMaster(s_transport, s_clock,
                                           arctic::liveIngestSink(),
                                           RESP_TIMEOUT_MS, POLL_TXN_DEADLINE_MS);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "macon_master initialised");
    return ESP_OK;
}

esp_err_t start()
{
    if (!s_initialized || s_master == nullptr) {
        ESP_LOGE(TAG, "start() before init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active.load()) return ESP_OK;

    ESP_LOGI(TAG, "Preflight: listening %d ms for other bus masters...", PREFLIGHT_MS);
    if (!s_master->preflight_bus_idle(PREFLIGHT_MS)) {
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

// Serialise a verified setpoint write through the bus mutex, then log the
// outcome. The library master owns the RX flush before/after the write.
static bool guarded_setpoint(arctic::MaconResult (arctic::MaconMaster::*fn)(int),
                             int celsius, const char *what)
{
    if (!s_active.load() || s_master == nullptr || s_bus_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    const arctic::MaconResult r = (s_master->*fn)(celsius);
    xSemaphoreGive(s_bus_mutex);

    if (r == arctic::MaconResult::Ok) {
        ESP_LOGI(TAG, "%s setpoint -> %d C (ACKed)", what, celsius);
        return true;
    }
    ESP_LOGW(TAG, "%s setpoint write failed: %s", what, arctic::macon_result_name(r));
    return false;
}

bool set_cooling_setpoint(int celsius)
{
    return guarded_setpoint(&arctic::MaconMaster::set_cooling_setpoint, celsius, "cooling");
}

bool set_hot_water_setpoint(int celsius)
{
    return guarded_setpoint(&arctic::MaconMaster::set_hot_water_setpoint, celsius, "hot-water");
}

bool write_register(uint16_t address, uint8_t value)
{
    if (!s_active.load() || s_master == nullptr || s_bus_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    const arctic::MaconResult r = s_master->write_register(address, value);
    xSemaphoreGive(s_bus_mutex);

    if (r == arctic::MaconResult::Ok) {
        ESP_LOGI(TAG, "register %u -> %u (ACKed)", (unsigned)address, (unsigned)value);
        return true;
    }
    ESP_LOGW(TAG, "register %u write failed: %s",
             (unsigned)address, arctic::macon_result_name(r));
    return false;
}

}  // namespace macon_master
