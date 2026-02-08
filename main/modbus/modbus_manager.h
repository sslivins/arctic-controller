/*
 * Modbus RTU Master for Arctic Heat Pump
 * RS-485 communication using ESP-IDF UART
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

namespace modbus {

// Initialize Modbus RTU master on RS-485
// Returns ESP_OK on success
esp_err_t init();

// Deinitialize Modbus
void deinit();

// Check if Modbus is initialized
bool isInitialized();

// Read holding registers (function code 0x03)
// slave_addr: Slave device address (1-247)
// start_reg: Starting register address
// num_regs: Number of registers to read (1-125)
// data: Output buffer (must be at least num_regs * 2 bytes)
// Returns ESP_OK on success, ESP_ERR_TIMEOUT on no response, ESP_ERR_INVALID_CRC on CRC error
esp_err_t readHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* data);

// Write single register (function code 0x06)
// slave_addr: Slave device address (1-247)
// reg_addr: Register address to write
// value: Value to write
// Returns ESP_OK on success
esp_err_t writeSingleRegister(uint8_t slave_addr, uint16_t reg_addr, uint16_t value);

// Write multiple registers (function code 0x10)
// slave_addr: Slave device address (1-247)
// start_reg: Starting register address
// num_regs: Number of registers to write (1-123)
// data: Values to write
// Returns ESP_OK on success
esp_err_t writeMultipleRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, const uint16_t* data);

// Get last communication error description
const char* getLastError();

// Get statistics
struct Stats {
    uint32_t tx_count;       // Total frames transmitted
    uint32_t rx_count;       // Total successful responses
    uint32_t timeout_count;  // Timeout errors
    uint32_t crc_error_count;// CRC errors
    uint32_t other_error_count; // Other errors
};

Stats getStats();
void resetStats();

}  // namespace modbus
