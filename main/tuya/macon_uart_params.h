#pragma once

// Maps the arctic-macon library's protocol-owned bus parameters
// (arctic::MACON_BUS_PARAMS — baud + character framing) onto an ESP-IDF
// uart_config_t. The *value* of the wire settings lives in the library (it is a
// property of the Macon protocol, not this board); only the *mechanism* of
// applying them to a UART driver lives here, so the transport never hardcodes a
// magic baud/parity of its own.

#include "driver/uart.h"
#include "macon_bus.h"

namespace arctic {

inline uart_word_length_t macon_uart_data_bits() {
    switch (MACON_BUS_PARAMS.data_bits) {
        case 5:  return UART_DATA_5_BITS;
        case 6:  return UART_DATA_6_BITS;
        case 7:  return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

inline uart_parity_t macon_uart_parity() {
    switch (MACON_BUS_PARAMS.parity) {
        case MaconParity::Even: return UART_PARITY_EVEN;
        case MaconParity::Odd:  return UART_PARITY_ODD;
        default:                return UART_PARITY_DISABLE;
    }
}

inline uart_stop_bits_t macon_uart_stop_bits() {
    return (MACON_BUS_PARAMS.stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

// Fully-populated uart_config_t for the Macon bus (flow control off, default
// source clock). Callers may override individual fields afterwards if needed.
inline uart_config_t macon_uart_config() {
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)MACON_BUS_PARAMS.baud;
    cfg.data_bits  = macon_uart_data_bits();
    cfg.parity     = macon_uart_parity();
    cfg.stop_bits  = macon_uart_stop_bits();
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    return cfg;
}

}  // namespace arctic
