/*
 * Modbus RTU Master for Arctic Heat Pump
 * Using ESP-IDF esp_modbus component
 */

#include "modbus_manager.h"
#include "arctic_registers.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "mbcontroller.h"
#include <string.h>

static const char* TAG = "modbus";

// Modbus function codes (from Modbus specification)
#define MB_FUNC_READ_HOLDING_REGISTER       (0x03)
#define MB_FUNC_WRITE_REGISTER              (0x06)
#define MB_FUNC_WRITE_MULTIPLE_REGISTERS    (0x10)

namespace modbus {

// State
static bool s_initialized = false;
static char s_last_error[64] = {0};
static Stats s_stats = {};

esp_err_t init() {
    if (s_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing Modbus RTU master using esp_modbus (2400 8E1)");
    
    // Initialize Modbus controller
    void* master_handle = nullptr;
    esp_err_t err = mbc_master_init(MB_PORT_SERIAL_MASTER, &master_handle);
    if (err != ESP_OK || master_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize Modbus master: %s", esp_err_to_name(err));
        snprintf(s_last_error, sizeof(s_last_error), "Init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Configure Modbus communication parameters
    // esp_modbus will install UART driver with these settings
    mb_communication_info_t comm_info = {};
    comm_info.port = UART_NUM_1;
    comm_info.mode = MB_MODE_RTU;
    comm_info.baudrate = arctic::BAUD_RATE;
    comm_info.parity = UART_PARITY_EVEN;
    
    err = mbc_master_setup((void*)&comm_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup Modbus master: %s", esp_err_to_name(err));
        snprintf(s_last_error, sizeof(s_last_error), "Setup failed: %s", esp_err_to_name(err));
        mbc_master_destroy();
        return err;
    }
    
    // Start Modbus controller (this installs the UART driver)
    err = mbc_master_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Modbus master: %s", esp_err_to_name(err));
        snprintf(s_last_error, sizeof(s_last_error), "Start failed: %s", esp_err_to_name(err));
        mbc_master_destroy();
        return err;
    }
    
    // NOW we can reconfigure UART - driver is installed after mbc_master_start()
    err = uart_set_pin(UART_NUM_1, 
                       arctic::RS485_TX_PIN, 
                       arctic::RS485_RX_PIN,
                       arctic::RS485_DIR_PIN,  // RTS pin for RS-485 direction control
                       UART_PIN_NO_CHANGE);    // CTS not used
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        snprintf(s_last_error, sizeof(s_last_error), "Pin config failed: %s", esp_err_to_name(err));
        mbc_master_destroy();
        return err;
    }
    
    // Set RS-485 half-duplex mode
    err = uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set RS-485 mode: %s", esp_err_to_name(err));
        snprintf(s_last_error, sizeof(s_last_error), "RS485 mode failed: %s", esp_err_to_name(err));
        mbc_master_destroy();
        return err;
    }
    
    s_initialized = true;
    resetStats();
    
    ESP_LOGI(TAG, "Modbus RTU master initialized (TX:%d, RX:%d, RTS:%d)",
             arctic::RS485_TX_PIN, arctic::RS485_RX_PIN, arctic::RS485_DIR_PIN);
    
    return ESP_OK;
}

void deinit() {
    if (!s_initialized) return;
    
    mbc_master_destroy();
    s_initialized = false;
    ESP_LOGI(TAG, "Modbus deinitialized");
}

bool isInitialized() {
    return s_initialized;
}

esp_err_t readHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* data) {
    if (!s_initialized) {
        snprintf(s_last_error, sizeof(s_last_error), "Modbus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (num_regs == 0 || num_regs > 125 || data == nullptr) {
        snprintf(s_last_error, sizeof(s_last_error), "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_stats.tx_count++;
    
    // Use esp_modbus send request API
    mb_param_request_t request = {
        .slave_addr = slave_addr,
        .command = MB_FUNC_READ_HOLDING_REGISTER,
        .reg_start = start_reg,
        .reg_size = num_regs
    };
    
    esp_err_t err = mbc_master_send_request(&request, (void*)data);
    
    if (err == ESP_OK) {
        s_stats.rx_count++;
        s_last_error[0] = '\0';
    } else if (err == ESP_ERR_TIMEOUT) {
        s_stats.timeout_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Timeout (no response)");
    } else if (err == ESP_ERR_INVALID_CRC) {
        s_stats.crc_error_count++;
        snprintf(s_last_error, sizeof(s_last_error), "CRC error");
    } else {
        s_stats.other_error_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Error: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t writeSingleRegister(uint8_t slave_addr, uint16_t reg_addr, uint16_t value) {
    if (!s_initialized) {
        snprintf(s_last_error, sizeof(s_last_error), "Modbus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_stats.tx_count++;
    
    mb_param_request_t request = {
        .slave_addr = slave_addr,
        .command = MB_FUNC_WRITE_REGISTER,
        .reg_start = reg_addr,
        .reg_size = 1
    };
    
    esp_err_t err = mbc_master_send_request(&request, (void*)&value);
    
    if (err == ESP_OK) {
        s_stats.rx_count++;
        s_last_error[0] = '\0';
    } else if (err == ESP_ERR_TIMEOUT) {
        s_stats.timeout_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Timeout");
    } else {
        s_stats.other_error_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Error: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t writeMultipleRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, const uint16_t* data) {
    if (!s_initialized) {
        snprintf(s_last_error, sizeof(s_last_error), "Modbus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (num_regs == 0 || num_regs > 123 || data == nullptr) {
        snprintf(s_last_error, sizeof(s_last_error), "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_stats.tx_count++;
    
    mb_param_request_t request = {
        .slave_addr = slave_addr,
        .command = MB_FUNC_WRITE_MULTIPLE_REGISTERS,
        .reg_start = start_reg,
        .reg_size = num_regs
    };
    
    esp_err_t err = mbc_master_send_request(&request, (void*)data);
    
    if (err == ESP_OK) {
        s_stats.rx_count++;
        s_last_error[0] = '\0';
    } else if (err == ESP_ERR_TIMEOUT) {
        s_stats.timeout_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Timeout");
    } else {
        s_stats.other_error_count++;
        snprintf(s_last_error, sizeof(s_last_error), "Error: %s", esp_err_to_name(err));
    }
    
    return err;
}

const char* getLastError() {
    return s_last_error;
}

Stats getStats() {
    return s_stats;
}

void resetStats() {
    memset(&s_stats, 0, sizeof(s_stats));
}

}  // namespace modbus
