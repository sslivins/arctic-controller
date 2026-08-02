/*
 * RS485 half-duplex UART transport for the Arctic/Macon Tuya bus.
 *
 * Implements arctic::MaconTransport (the byte pipe that MaconLink drives) over
 * the Tab5's UART1 RS485 transceiver. Unlike the passive listener, this path
 * TRANSMITS: it is used only in active-master mode, where the Tab5 is the sole
 * bus master and the OEM controller must be physically disconnected.
 *
 * Turnaround is handled in hardware via UART_MODE_RS485_HALF_DUPLEX: the UART
 * driver raises RTS (wired to the RS485 driver-enable / DIR pin) for the exact
 * duration of the transmission and lowers it once the final stop bit has
 * shifted out. This avoids the truncation/echo races of manual GPIO toggling.
 *
 * Wire settings (validated against the real bus): 4800 baud, 8-E-1.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "macon_link.h"   // arctic::MaconTransport

namespace tuya {

class MaconUartTransport : public arctic::MaconTransport {
public:
    // Install and configure UART1 in RS485 half-duplex mode (4800 8E1) with
    // the driver-enable on RS485_DIR_PIN (as RTS). Idempotent.
    esp_err_t init();

    // arctic::MaconTransport -------------------------------------------------
    // Write exactly n bytes, blocking until the frame is fully on the wire and
    // the RS485 turnaround has completed. Returns n on success, or a negative
    // value if the write was short or TX completion timed out.
    int write(const uint8_t *data, size_t n) override;

    // Read up to n bytes, blocking at most timeout_ms. Returns the count
    // (0 on timeout) or a negative value on a driver error.
    int read(uint8_t *buf, size_t n, int timeout_ms) override;

    // Discard any buffered RX bytes. Only safe to call when the bus is known
    // idle (e.g. immediately before issuing a request, or after a failed
    // transaction) so a genuine response is never dropped.
    void flush_rx();

private:
    bool initialized_ = false;
};

}  // namespace tuya
