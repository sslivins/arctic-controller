/*
 * RS485 half-duplex UART transport implementation. See macon_uart_transport.h.
 */
#include "macon_uart_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "heatpump_types.h"   // RS485_TX_PIN / RS485_RX_PIN / RS485_DIR_PIN

static const char *TAG = "macon_tx";

namespace tuya {

// Master shares the passive listener's port and wire settings. Only one of the
// two runs (the bus mode is a Kconfig choice), so there is no UART1 conflict.
static constexpr uart_port_t UART_PORT   = UART_NUM_1;
static constexpr int         TUYA_BAUD   = 4800;
static constexpr size_t      RX_BUF_SIZE = 2048;
static constexpr size_t      TX_BUF_SIZE = 512;

// Upper bound for uart_wait_tx_done(). The longest frame we send is a 9-byte
// read request; even a full 67-byte response-sized frame at 4800 8-E-1
// (~10 bits/byte => ~2.3 ms/byte) is < 160 ms. 250 ms gives ample headroom.
static constexpr int         TX_DONE_TIMEOUT_MS = 250;

esp_err_t MaconUartTransport::init()
{
    if (initialized_) return ESP_OK;

    // Drive the RS485 direction/DE line LOW (receive) before the UART takes it
    // over as RTS, so the transceiver never spuriously drives the bus during
    // boot/init. A pull-down keeps it safe if the pin floats before this runs.
    gpio_config_t dir_cfg = {};
    dir_cfg.pin_bit_mask = 1ULL << arctic::RS485_DIR_PIN;
    dir_cfg.mode         = GPIO_MODE_OUTPUT;
    dir_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    dir_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    dir_cfg.intr_type    = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&dir_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DIR gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)arctic::RS485_DIR_PIN, 0);

    uart_config_t cfg = {};
    cfg.baud_rate  = TUYA_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_EVEN;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    err = uart_driver_install(UART_PORT, RX_BUF_SIZE, TX_BUF_SIZE, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    // TX, RX, RTS(=DE/DIR), CTS(unused). In RS485 half-duplex mode the driver
    // controls RTS automatically as the transceiver driver-enable.
    err = uart_set_pin(UART_PORT,
                       arctic::RS485_TX_PIN,
                       arctic::RS485_RX_PIN,
                       arctic::RS485_DIR_PIN,   // RTS -> RS485 DE
                       UART_PIN_NO_CHANGE);     // CTS unused
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_mode(RS485) failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized_ = true;
    ESP_LOGI(TAG,
             "RS485 half-duplex master UART ready (%d 8E1, TX=GPIO%d RX=GPIO%d DE=GPIO%d)",
             TUYA_BAUD, arctic::RS485_TX_PIN, arctic::RS485_RX_PIN,
             arctic::RS485_DIR_PIN);
    return ESP_OK;
}

int MaconUartTransport::write(const uint8_t *data, size_t n)
{
    if (!initialized_ || data == nullptr) return -1;
    if (n == 0) return 0;

    const int written = uart_write_bytes(UART_PORT, data, n);
    if (written < 0 || static_cast<size_t>(written) != n) {
        ESP_LOGE(TAG, "uart_write_bytes short: %d/%u", written, (unsigned)n);
        return written < 0 ? written : -1;
    }
    // Block until the frame (and the hardware RS485 DE turnaround) is fully on
    // the wire. Lowering DE too early would truncate the last byte; letting the
    // driver report a timeout here lets the caller abort the transaction.
    const esp_err_t err = uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_wait_tx_done failed: %s", esp_err_to_name(err));
        return -1;
    }
    return written;
}

int MaconUartTransport::read(uint8_t *buf, size_t n, int timeout_ms)
{
    if (!initialized_ || buf == nullptr) return -1;
    if (timeout_ms < 0) timeout_ms = 0;
    // uart_read_bytes returns the number of bytes read (0 on timeout) or a
    // negative value on a driver error -- matching the MaconTransport contract.
    return uart_read_bytes(UART_PORT, buf, n, pdMS_TO_TICKS(timeout_ms));
}

void MaconUartTransport::flush_rx()
{
    if (initialized_) uart_flush_input(UART_PORT);
}

}  // namespace tuya
