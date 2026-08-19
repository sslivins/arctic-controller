/*
 * Passive Tuya bus listener implementation. See tuya_listener.h.
 *
 * This file is only the platform glue: it installs an RX-only UART on the
 * RS485 pins and pumps the raw bytes into the arctic-macon library, which owns
 * ALL framing/register/window decoding. The controller (and this shim) carry no
 * Tuya/Macon wire knowledge — bytes go in, semantic state comes out.
 */
#include "tuya_listener.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "macon_bus_config.h"
#include "macon_uart_params.h"
#include "heatpump_controller.h"  // arctic::feedListenerBytes / getListenerStats

static const char *TAG = "tuya_listen";

namespace tuya {

// ---------------------------------------------------------------------------
// Wire configuration
// ---------------------------------------------------------------------------
//
// The real Arctic RS485 bus runs at the library-owned Macon wire settings
// (arctic::MACON_BUS_PARAMS — 4800 8-E-1).
static constexpr uart_port_t UART_PORT   = UART_NUM_1;
static constexpr size_t      RX_BUF_SIZE = 2048;   // driver ring buffer
static constexpr size_t      READ_CHUNK  = 256;    // per read_bytes slice

static bool          s_initialized = false;
static TaskHandle_t  s_task        = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// Receive task
// ---------------------------------------------------------------------------
//
// Reads raw bytes and hands them to the library decoder. The library owns the
// frame accumulator, resync state machine, statistics, and register ingest.
//
static void rx_task(void *param)
{
    static uint8_t buf[READ_CHUNK];
    uint32_t       last_idle_log = now_ms();
    uint32_t       frames_at_log = 0;

    ESP_LOGI(TAG, "Listener task started (RX-only, %d baud 8E1, RX=GPIO%d, DIR=GPIO%d held low)",
             (int)arctic::MACON_BUS_PARAMS.baud, arctic::RS485_RX_PIN, arctic::RS485_DIR_PIN);

    for (;;) {
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            arctic::feedListenerBytes(buf, (size_t)n);
        }

        // Heartbeat every 5s so we can confirm the task is alive on the bench
        // even when the RS485 bus is not connected yet.
        uint32_t t = now_ms();
        if (t - last_idle_log >= 5000) {
            ListenerStats snap = listener_get_stats();
            if (snap.frames_ok == frames_at_log) {
                ESP_LOGI(TAG, "listening... no frames yet (bytes_rx=%lu resync=%lu)",
                         (unsigned long)snap.bytes_rx, (unsigned long)snap.resync);
            } else {
                ESP_LOGI(TAG,
                         "frames_ok=%lu (req=%lu resp=%lu) bytes_rx=%lu chk_err=%lu resync=%lu",
                         (unsigned long)snap.frames_ok,
                         (unsigned long)snap.req_frames,
                         (unsigned long)snap.resp_frames,
                         (unsigned long)snap.bytes_rx,
                         (unsigned long)snap.checksum_err,
                         (unsigned long)snap.resync);
            }
            frames_at_log = snap.frames_ok;
            last_idle_log = t;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t listener_init()
{
    if (s_initialized) return ESP_OK;

    // Force the RS485 direction pin low: transceiver driver disabled ->
    // permanently in receive. This is what makes the listener passive.
    gpio_config_t dir_cfg = {};
    dir_cfg.pin_bit_mask = 1ULL << arctic::RS485_DIR_PIN;
    dir_cfg.mode         = GPIO_MODE_OUTPUT;
    dir_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    dir_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dir_cfg.intr_type    = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&dir_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DIR gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)arctic::RS485_DIR_PIN, 0);

    uart_config_t cfg = arctic::macon_uart_config();

    err = uart_driver_install(UART_PORT, RX_BUF_SIZE, 0 /*tx*/, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    // RX only: leave TX/RTS/CTS unrouted so the ESP can never drive the bus.
    err = uart_set_pin(UART_PORT,
                       UART_PIN_NO_CHANGE,     // TX intentionally unconnected
                       arctic::RS485_RX_PIN,   // RX
                       UART_PIN_NO_CHANGE,     // RTS (DIR handled manually)
                       UART_PIN_NO_CHANGE);    // CTS
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Passive Tuya listener initialized (%d baud 8E1, RX=GPIO%d)",
             (int)arctic::MACON_BUS_PARAMS.baud, arctic::RS485_RX_PIN);
    return ESP_OK;
}

void listener_start()
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "listener_start() called before listener_init()");
        return;
    }
    if (s_task) return;
    xTaskCreate(rx_task, "tuya_listen", 4096, nullptr, 5, &s_task);
}

ListenerStats listener_get_stats()
{
    // Delegate to the library-owned statistics; the controller carries no
    // frame-decode state of its own.
    const arctic::MaconListenerStats s = arctic::getListenerStats();
    ListenerStats out;
    out.bytes_rx      = s.bytes_rx;
    out.frames_ok     = s.frames_ok;
    out.req_frames    = s.req_frames;
    out.resp_frames   = s.resp_frames;
    out.checksum_err  = s.checksum_err;
    out.resync        = s.resync;
    out.last_frame_ms = s.last_frame_ms;
    return out;
}

}  // namespace tuya
