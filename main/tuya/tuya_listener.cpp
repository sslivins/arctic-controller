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
#include "freertos/queue.h"
#include <cstdio>
#include "esp_log.h"
#include "esp_heap_caps.h"
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

static constexpr int         EVT_QUEUE_LEN = 32;

static bool          s_initialized = false;
static TaskHandle_t  s_task        = nullptr;
static QueueHandle_t s_uart_evq    = nullptr;

// RX-path diagnostics, counted at the platform shim so they are independent
// of the library decoder: pin edges prove the signal reaches the pad, driver
// bytes/events prove the UART peripheral is sampling it.
static volatile uint32_t s_rx_edges    = 0;
static uint32_t          s_drv_bytes   = 0;
static uint32_t          s_ev_data     = 0;
static uint32_t          s_ev_break    = 0;
static uint32_t          s_ev_frame    = 0;
static uint32_t          s_ev_parity   = 0;
static uint32_t          s_ev_fifo_ovf = 0;
static uint32_t          s_ev_buf_full = 0;
static uint32_t          s_ev_other    = 0;

static void IRAM_ATTR rx_edge_isr(void *)
{
    s_rx_edges = s_rx_edges + 1;
}

static void drain_uart_events()
{
    if (!s_uart_evq) return;
    uart_event_t ev;
    while (xQueueReceive(s_uart_evq, &ev, 0) == pdTRUE) {
        switch (ev.type) {
            case UART_DATA:        ++s_ev_data;     break;
            case UART_BREAK:       ++s_ev_break;    break;
            case UART_FRAME_ERR:   ++s_ev_frame;    break;
            case UART_PARITY_ERR:  ++s_ev_parity;   break;
            case UART_FIFO_OVF:    ++s_ev_fifo_ovf; uart_flush_input(UART_PORT); break;
            case UART_BUFFER_FULL: ++s_ev_buf_full; uart_flush_input(UART_PORT); break;
            default:               ++s_ev_other;    break;
        }
    }
}

// Samples the raw pad level for ~20ms. An idle RS485 receiver output sits
// high; a live 4800-baud bus should show some low samples.
static uint32_t sample_rx_low_count(uint32_t *total_out)
{
    uint32_t lows = 0, total = 0;
    const int64_t end = esp_timer_get_time() + 20000;
    while (esp_timer_get_time() < end) {
        if (gpio_get_level((gpio_num_t)arctic::RS485_RX_PIN) == 0) ++lows;
        ++total;
    }
    *total_out = total;
    return lows;
}

static void dump_rs485_pins(const char *when)
{
    ESP_LOGI(TAG, "IO config dump (%s) for TX=%d RX=%d DIR=%d:", when,
             arctic::RS485_TX_PIN, arctic::RS485_RX_PIN, arctic::RS485_DIR_PIN);
    gpio_dump_io_configuration(stdout, (1ULL << arctic::RS485_TX_PIN) |
                                       (1ULL << arctic::RS485_RX_PIN) |
                                       (1ULL << arctic::RS485_DIR_PIN));
}

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

    dump_rs485_pins("task start");
    uint32_t hb_count = 0;

    for (;;) {
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            s_drv_bytes += (uint32_t)n;
            arctic::feedListenerBytes(buf, (size_t)n);
        }
        drain_uart_events();

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
            uint32_t samples = 0;
            const uint32_t lows = sample_rx_low_count(&samples);
            size_t buffered = 0;
            uart_get_buffered_data_len(UART_PORT, &buffered);
            uint32_t baud = 0;
            uart_get_baudrate(UART_PORT, &baud);
            ESP_LOGI(TAG,
                     "rxdiag: edges=%lu lvl=%d low=%lu/%lu drv_bytes=%lu buffered=%u baud=%lu "
                     "ev{data=%lu brk=%lu frm=%lu par=%lu ovf=%lu full=%lu oth=%lu}",
                     (unsigned long)s_rx_edges,
                     gpio_get_level((gpio_num_t)arctic::RS485_RX_PIN),
                     (unsigned long)lows, (unsigned long)samples,
                     (unsigned long)s_drv_bytes, (unsigned)buffered, (unsigned long)baud,
                     (unsigned long)s_ev_data, (unsigned long)s_ev_break,
                     (unsigned long)s_ev_frame, (unsigned long)s_ev_parity,
                     (unsigned long)s_ev_fifo_ovf, (unsigned long)s_ev_buf_full,
                     (unsigned long)s_ev_other);
            // Re-dump once later in boot to catch anything (WiFi/BSP bring-up)
            // that re-muxes the RS485 pins after the listener configured them.
            if (++hb_count == 6) dump_rs485_pins("t+30s");
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

    err = uart_driver_install(UART_PORT, RX_BUF_SIZE, 0 /*tx*/, EVT_QUEUE_LEN, &s_uart_evq, 0);
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

    // Edge counter on the RX pad. Only the interrupt type is touched, not the
    // pin mux, so the UART keeps receiving through the GPIO matrix.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
    } else {
        gpio_set_intr_type((gpio_num_t)arctic::RS485_RX_PIN, GPIO_INTR_ANYEDGE);
        isr_err = gpio_isr_handler_add((gpio_num_t)arctic::RS485_RX_PIN, rx_edge_isr, nullptr);
        if (isr_err == ESP_OK) isr_err = gpio_intr_enable((gpio_num_t)arctic::RS485_RX_PIN);
        if (isr_err != ESP_OK) {
            ESP_LOGW(TAG, "RX edge counter unavailable: %s", esp_err_to_name(isr_err));
        }
    }
    dump_rs485_pins("after init");

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
    // Internal stack: the RX task services the UART at line rate.
    if (xTaskCreate(rx_task, "tuya_listen", 4096, nullptr, 5, &s_task) != pdPASS) {
        // Silent failure leaves s_task null, so every later start() retries
        // while the heat pump appears simply not to respond.
        ESP_LOGE(TAG, "Failed to create tuya listener task (largest free internal block=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_task = nullptr;
    }
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
