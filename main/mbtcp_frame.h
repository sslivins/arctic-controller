/*
 * Minimal Modbus TCP client framing: read holding (03) / input (04) registers.
 * Pure byte building and parsing; the socket side lives in ext_temp_sensors.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mbtcp {

constexpr uint8_t kFcReadHolding = 0x03;
constexpr uint8_t kFcReadInput = 0x04;
constexpr uint16_t kMaxReadCount = 125;
constexpr size_t kRequestLen = 12;
constexpr size_t kHeaderLen = 7;  // MBAP header including the unit ID
constexpr size_t kMaxResponseLen = kHeaderLen + 2 + 2 * kMaxReadCount;

// Builds a read request. Returns false for a bad function code or count.
bool build_read(uint16_t transaction, uint8_t unit, uint8_t fc, uint16_t start, uint16_t count,
                uint8_t out[kRequestLen]);

// Total length of the frame whose first kHeaderLen-1 (6) bytes are in `buf`,
// or 0 if the header is not yet complete or invalid.
size_t frame_length(const uint8_t* buf, size_t len);

enum class Parse : uint8_t {
    Ok,
    Incomplete,
    BadProtocol,
    WrongTransaction,
    WrongUnit,
    WrongFunction,
    BadLength,
    Exception,
};

// Parses a complete response to build_read(transaction, unit, fc, _, count).
// On Parse::Exception, *exception_code holds the device's exception code.
Parse parse_read(const uint8_t* buf, size_t len, uint16_t transaction, uint8_t unit, uint8_t fc,
                 uint16_t count, uint16_t* regs_out, uint8_t* exception_code);

const char* parse_name(Parse p);

}  // namespace mbtcp
