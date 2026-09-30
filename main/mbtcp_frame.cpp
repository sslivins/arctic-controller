#include "mbtcp_frame.h"

namespace mbtcp {

bool build_read(uint16_t transaction, uint8_t unit, uint8_t fc, uint16_t start, uint16_t count,
                uint8_t out[kRequestLen]) {
    if (fc != kFcReadHolding && fc != kFcReadInput) return false;
    if (count == 0 || count > kMaxReadCount) return false;
    if (static_cast<uint32_t>(start) + count - 1 > 0xFFFF) return false;
    out[0] = static_cast<uint8_t>(transaction >> 8);
    out[1] = static_cast<uint8_t>(transaction);
    out[2] = 0;  // protocol ID
    out[3] = 0;
    out[4] = 0;  // length: unit + 5-byte PDU
    out[5] = 6;
    out[6] = unit;
    out[7] = fc;
    out[8] = static_cast<uint8_t>(start >> 8);
    out[9] = static_cast<uint8_t>(start);
    out[10] = static_cast<uint8_t>(count >> 8);
    out[11] = static_cast<uint8_t>(count);
    return true;
}

size_t frame_length(const uint8_t* buf, size_t len) {
    if (len < 6) return 0;
    uint16_t follow = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
    // Unit ID plus at least a function code and one byte; never larger than
    // the biggest legal read response.
    if (follow < 3 || follow > kMaxResponseLen - 6) return 0;
    return 6 + follow;
}

Parse parse_read(const uint8_t* buf, size_t len, uint16_t transaction, uint8_t unit, uint8_t fc,
                 uint16_t count, uint16_t* regs_out, uint8_t* exception_code) {
    size_t total = frame_length(buf, len);
    if (len < 6) return Parse::Incomplete;
    if (buf[2] != 0 || buf[3] != 0) return Parse::BadProtocol;
    if (total == 0) return Parse::BadLength;
    if (len < total) return Parse::Incomplete;
    if (static_cast<uint16_t>((buf[0] << 8) | buf[1]) != transaction) {
        return Parse::WrongTransaction;
    }
    if (buf[6] != unit) return Parse::WrongUnit;
    uint8_t rfc = buf[7];
    if (rfc == (fc | 0x80)) {
        if (total != kHeaderLen + 2) return Parse::BadLength;
        if (exception_code) *exception_code = buf[8];
        return Parse::Exception;
    }
    if (rfc != fc) return Parse::WrongFunction;
    uint8_t bytes = buf[8];
    if (bytes != 2 * count || total != kHeaderLen + 2 + bytes) return Parse::BadLength;
    for (uint16_t i = 0; i < count; ++i) {
        regs_out[i] = static_cast<uint16_t>((buf[9 + 2 * i] << 8) | buf[10 + 2 * i]);
    }
    return Parse::Ok;
}

const char* parse_name(Parse p) {
    switch (p) {
        case Parse::Ok: return "ok";
        case Parse::Incomplete: return "incomplete";
        case Parse::BadProtocol: return "bad_protocol";
        case Parse::WrongTransaction: return "wrong_transaction";
        case Parse::WrongUnit: return "wrong_unit";
        case Parse::WrongFunction: return "wrong_function";
        case Parse::BadLength: return "bad_length";
        case Parse::Exception: return "exception";
    }
    return "unknown";
}

}  // namespace mbtcp
