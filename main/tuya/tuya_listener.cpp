/*
 * Passive Tuya bus listener implementation. See tuya_listener.h.
 */
#include "tuya_listener.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "tuya_codec.h"
#include "arctic_registers.h"   // RS485_TX_PIN / RS485_RX_PIN / RS485_DIR_PIN
#include "arctic_heatpump.h"    // arctic::feedRegisterWindow

static const char *TAG = "tuya_listen";

namespace tuya {

// ---------------------------------------------------------------------------
// Wire configuration
// ---------------------------------------------------------------------------
//
// The real Arctic RS485 bus runs at 4800 baud 8-E-1 (NOT the 2400 that older
// docs / arctic_registers.h::BAUD_RATE claim). Keep this local so the passive
// listener is unaffected by the Modbus constant.
//
static constexpr uart_port_t UART_PORT   = UART_NUM_1;
static constexpr int         TUYA_BAUD   = 4800;
static constexpr size_t      RX_BUF_SIZE = 2048;   // driver ring buffer
static constexpr size_t      ACC_SIZE    = 512;    // decode accumulator
static constexpr size_t      MAX_HEXBUF  = 200;    // hex string scratch (58 regs*3 + slack)

static bool          s_initialized = false;
static ListenerStats s_stats       = {};
static portMUX_TYPE  s_mux         = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t  s_task        = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Format up to `len` bytes as space-separated hex into `out`.
static void hex_dump(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < out_cap; ++i) {
        pos += snprintf(out + pos, out_cap - pos, "%02X ", data[i]);
    }
    if (out_cap > 0) out[(pos > 0 && pos < out_cap) ? pos - 1 : 0] = '\0';
}

static void log_frame(const uint8_t *frame, const tuya_codec::ParsedFrame &pf)
{
    static char hexbuf[MAX_HEXBUF];
    if (pf.dir == tuya_codec::DIR_REQUEST) {
        hex_dump(frame, pf.frame_len, hexbuf, sizeof(hexbuf));
        ESP_LOGI(TAG, "REQ  reg%u cnt%u  [%s]",
                 (unsigned)pf.window->reg_base, (unsigned)pf.field_b, hexbuf);
    } else {
        // For responses show the register payload (after the window prefix).
        const uint8_t *regs = pf.payload + pf.window->prefix_len;
        size_t reg_len = (pf.payload_len > pf.window->prefix_len)
                             ? pf.payload_len - pf.window->prefix_len
                             : 0;
        hex_dump(regs, reg_len, hexbuf, sizeof(hexbuf));
        ESP_LOGI(TAG, "RESP reg%u cnt%u  regs[%s]",
                 (unsigned)pf.window->reg_base, (unsigned)pf.field_b, hexbuf);
    }
}

// ---------------------------------------------------------------------------
// Decode accumulator drain
// ---------------------------------------------------------------------------
//
// Consumes as many complete frames as possible from the front of `acc`.
// Returns the number of bytes consumed; the caller shifts the remainder.
//
static size_t drain(const uint8_t *acc, size_t acc_len)
{
    size_t consumed = 0;
    while (acc_len - consumed >= tuya_codec::HDR_LEN) {
        const uint8_t *p   = acc + consumed;
        size_t         rem = acc_len - consumed;

        // Locate the next plausible frame start (validated header).
        size_t start = tuya_codec::find_frame_start(p, rem);
        if (start == rem) {
            // No frame start in the buffer. Drop everything except a possible
            // partial magic at the tail (keep last HDR_LEN-1 bytes).
            size_t keep = (rem < tuya_codec::HDR_LEN - 1) ? rem
                                                          : tuya_codec::HDR_LEN - 1;
            consumed = acc_len - keep;
            break;
        }
        if (start > 0) {
            // Junk before the frame start -> resync.
            portENTER_CRITICAL(&s_mux);
            s_stats.resync++;
            portEXIT_CRITICAL(&s_mux);
            consumed += start;
            continue;
        }

        tuya_codec::ParsedFrame pf;
        tuya_codec::ParseResult r =
            tuya_codec::parse_frame(acc + consumed, acc_len - consumed, pf);

        if (r == tuya_codec::ParseResult::OK) {
            portENTER_CRITICAL(&s_mux);
            s_stats.frames_ok++;
            if (pf.dir == tuya_codec::DIR_REQUEST) s_stats.req_frames++;
            else                                   s_stats.resp_frames++;
            s_stats.last_frame_ms = now_ms();
            portEXIT_CRITICAL(&s_mux);
            log_frame(acc + consumed, pf);

            // Feed response payloads (heat pump -> controller) into the state.
            if (pf.dir == tuya_codec::DIR_RESPONSE && pf.window &&
                pf.payload_len > pf.window->prefix_len) {
                arctic::feedRegisterWindow(
                    pf.window->reg_base,
                    pf.payload + pf.window->prefix_len,
                    pf.payload_len - pf.window->prefix_len);
            }
            // Catalog the FULL payload (incl. any window prefix bytes that the
            // feed strips) for diagnostics.
            if (pf.dir == tuya_codec::DIR_RESPONSE) {
                arctic::recordObservedWindow(pf.field_a, pf.field_b, 1,
                                             pf.payload, pf.payload_len);
            }
            consumed += pf.frame_len;
        } else if (r == tuya_codec::ParseResult::UNKNOWN_WINDOW) {
            // Valid framing + checksum but a window the codec doesn't map. This
            // is exactly how a not-yet-decoded register block (e.g. compressor
            // frequency) shows up. Catalog it and skip the whole frame.
            portENTER_CRITICAL(&s_mux);
            s_stats.frames_ok++;
            s_stats.resp_frames++;
            s_stats.last_frame_ms = now_ms();
            portEXIT_CRITICAL(&s_mux);
            if (pf.dir == tuya_codec::DIR_RESPONSE) {
                arctic::recordObservedWindow(pf.field_a, pf.field_b, 0,
                                             pf.payload, pf.payload_len);
            }
            ESP_LOGW(TAG, "UNKNOWN window addr=%u count=%u (cataloged)",
                     (unsigned)pf.field_a, (unsigned)pf.field_b);
            consumed += pf.frame_len;
        } else if (r == tuya_codec::ParseResult::TRUNCATED) {
            // Wait for more bytes before this frame can be parsed.
            break;
        } else {
            // Header looked valid but checksum (or length) failed. Skip past
            // the magic and resync.
            if (r == tuya_codec::ParseResult::BAD_CHECKSUM) {
                portENTER_CRITICAL(&s_mux);
                s_stats.checksum_err++;
                portEXIT_CRITICAL(&s_mux);
            }
            consumed += 2;
        }
    }
    return consumed;
}

// ---------------------------------------------------------------------------
// Receive task
// ---------------------------------------------------------------------------

static void rx_task(void *param)
{
    static uint8_t acc[ACC_SIZE];
    size_t         acc_len       = 0;
    uint32_t       last_idle_log = now_ms();
    uint32_t       frames_at_log = 0;

    ESP_LOGI(TAG, "Listener task started (RX-only, %d baud 8E1, RX=GPIO%d, DIR=GPIO%d held low)",
             TUYA_BAUD, arctic::RS485_RX_PIN, arctic::RS485_DIR_PIN);

    for (;;) {
        int n = uart_read_bytes(UART_PORT, acc + acc_len, ACC_SIZE - acc_len,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            acc_len += (size_t)n;
            portENTER_CRITICAL(&s_mux);
            s_stats.bytes_rx += (uint32_t)n;
            portEXIT_CRITICAL(&s_mux);

            size_t consumed = drain(acc, acc_len);
            if (consumed > 0 && consumed <= acc_len) {
                memmove(acc, acc + consumed, acc_len - consumed);
                acc_len -= consumed;
            }
            // Safety valve: if the accumulator is full and nothing drained,
            // the stream is garbage -> reset.
            if (acc_len == ACC_SIZE) {
                ESP_LOGW(TAG, "Accumulator full with no frame; resetting");
                acc_len = 0;
                portENTER_CRITICAL(&s_mux);
                s_stats.resync++;
                portEXIT_CRITICAL(&s_mux);
            }
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

    uart_config_t cfg = {};
    cfg.baud_rate = TUYA_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_EVEN;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

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
             TUYA_BAUD, arctic::RS485_RX_PIN);
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
    ListenerStats snap;
    portENTER_CRITICAL(&s_mux);
    snap = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return snap;
}

}  // namespace tuya
