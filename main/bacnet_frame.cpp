#include "bacnet_frame.h"

#include <string.h>

namespace bacnet {
namespace {

constexpr uint8_t kBvlcType = 0x81;
constexpr uint8_t kBvlcOriginalUnicast = 0x0A;
constexpr uint8_t kBvlcOriginalBroadcast = 0x0B;
constexpr uint8_t kNpduVersion = 0x01;
constexpr uint8_t kNpduExpectingReply = 0x04;
constexpr uint8_t kConfirmedRequest = 0x00;
constexpr uint8_t kComplexAck = 0x30;
constexpr uint8_t kErrorPdu = 0x50;
constexpr uint8_t kRejectPdu = 0x60;
constexpr uint8_t kAbortPdu = 0x70;
constexpr uint8_t kMaxSegmentsMaxApdu = 0x05;  // no segmentation accepted, max APDU 1476
constexpr uint8_t kServiceReadProperty = 12;
constexpr uint8_t kServiceReadPropertyMultiple = 14;
constexpr uint8_t kServiceIAm = 0;
constexpr uint8_t kServiceWhoIs = 8;
constexpr uint32_t kArrayAll = 0xFFFFFFFFu;

bool context_tag(uint8_t* out, size_t cap, size_t* pos, uint8_t tag, uint32_t value) {
    uint8_t bytes[4];
    size_t n = 1;
    if (value <= 0xFF) {
        bytes[0] = static_cast<uint8_t>(value);
    } else if (value <= 0xFFFF) {
        n = 2;
        bytes[0] = static_cast<uint8_t>(value >> 8);
        bytes[1] = static_cast<uint8_t>(value);
    } else if (value <= 0xFFFFFF) {
        n = 3;
        bytes[0] = static_cast<uint8_t>(value >> 16);
        bytes[1] = static_cast<uint8_t>(value >> 8);
        bytes[2] = static_cast<uint8_t>(value);
    } else {
        n = 4;
        bytes[0] = static_cast<uint8_t>(value >> 24);
        bytes[1] = static_cast<uint8_t>(value >> 16);
        bytes[2] = static_cast<uint8_t>(value >> 8);
        bytes[3] = static_cast<uint8_t>(value);
    }
    if (*pos + 1 + n > cap) return false;
    out[(*pos)++] = static_cast<uint8_t>((tag << 4) | n | 0x08);
    memcpy(out + *pos, bytes, n);
    *pos += n;
    return true;
}

bool context_object_id(uint8_t* out, size_t cap, size_t* pos, uint8_t tag, ObjectType type,
                       uint32_t instance) {
    if (instance > kDeviceWildcard || *pos + 5 > cap) return false;
    out[(*pos)++] = static_cast<uint8_t>((tag << 4) | 4 | 0x08);
    uint32_t oid = object_id_word(type, instance);
    out[(*pos)++] = static_cast<uint8_t>(oid >> 24);
    out[(*pos)++] = static_cast<uint8_t>(oid >> 16);
    out[(*pos)++] = static_cast<uint8_t>(oid >> 8);
    out[(*pos)++] = static_cast<uint8_t>(oid);
    return true;
}

bool opening(uint8_t* out, size_t cap, size_t* pos, uint8_t tag) {
    if (*pos + 1 > cap) return false;
    out[(*pos)++] = static_cast<uint8_t>((tag << 4) | 0x0E);
    return true;
}

bool closing(uint8_t* out, size_t cap, size_t* pos, uint8_t tag) {
    if (*pos + 1 > cap) return false;
    out[(*pos)++] = static_cast<uint8_t>((tag << 4) | 0x0F);
    return true;
}

bool begin(uint8_t invoke, uint8_t service, uint8_t* out, size_t cap, size_t* pos) {
    if (cap < 10) return false;
    *pos = 0;
    out[(*pos)++] = kBvlcType;
    out[(*pos)++] = kBvlcOriginalUnicast;
    out[(*pos)++] = 0;
    out[(*pos)++] = 0;
    out[(*pos)++] = kNpduVersion;
    out[(*pos)++] = kNpduExpectingReply;
    out[(*pos)++] = kConfirmedRequest;
    out[(*pos)++] = kMaxSegmentsMaxApdu;
    out[(*pos)++] = invoke;
    out[(*pos)++] = service;
    return true;
}

bool finish(uint8_t* out, size_t pos, size_t* out_len) {
    if (pos > kMaxFrame) return false;
    out[2] = static_cast<uint8_t>(pos >> 8);
    out[3] = static_cast<uint8_t>(pos);
    *out_len = pos;
    return true;
}

uint32_t read_be(const uint8_t* p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

struct Header {
    uint8_t pdu = 0;
    uint8_t invoke = 0;
    uint8_t service = 0;
    size_t pos = 0;
};

Parse parse_header(const uint8_t* buf, size_t len, uint8_t invoke, uint8_t service, Header* h,
                   ErrorInfo* err) {
    if (len < 7) return Parse::Incomplete;
    if (buf[0] != kBvlcType || buf[1] != kBvlcOriginalUnicast) return Parse::BadFrame;
    uint16_t total = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    if (total < 7 || total > len) return total > len ? Parse::Incomplete : Parse::BadFrame;
    if (buf[4] != kNpduVersion) return Parse::BadFrame;
    if ((buf[5] & 0x28) != 0) return Parse::BadFrame;
    h->pdu = buf[6] & 0xF0;
    if (h->pdu == kComplexAck) {
        if (total < 9) return Parse::Incomplete;
        h->invoke = buf[7];
        h->service = buf[8];
        h->pos = 9;
        if (h->invoke != invoke) return Parse::WrongInvoke;
        if (h->service != service) return Parse::WrongService;
        return Parse::Ok;
    }
    if (h->pdu == kErrorPdu) {
        if (total < 9) return Parse::Incomplete;
        h->invoke = buf[7];
        h->service = buf[8];
        h->pos = 9;
        if (h->invoke != invoke) return Parse::WrongInvoke;
        if (err) {
            Value v;
            // Best-effort: BACnet Error encodes class/code as enumerated values.
            size_t p = h->pos;
            auto read_enum = [&](uint8_t* out) -> bool {
                if (p >= total || (buf[p] >> 4) != 9) return false;
                size_t n = buf[p++] & 0x07;
                if (n == 5) {
                    if (p >= total) return false;
                    n = buf[p++];
                }
                if (n == 0 || n > 4 || p + n > total) return false;
                *out = static_cast<uint8_t>(read_be(buf + p, n));
                p += n;
                return true;
            };
            read_enum(&err->error_class);
            read_enum(&err->error_code);
            (void)v;
        }
        return Parse::Error;
    }
    if (h->pdu == kRejectPdu || h->pdu == kAbortPdu) {
        if (total < 9) return Parse::Incomplete;
        h->invoke = buf[7];
        if (h->invoke != invoke) return Parse::WrongInvoke;
        if (err) err->reason = buf[8];
        return h->pdu == kRejectPdu ? Parse::Reject : Parse::Abort;
    }
    return Parse::Unsupported;
}

bool is_open(const uint8_t b, uint8_t tag) { return b == static_cast<uint8_t>((tag << 4) | 0x0E); }
bool is_close(const uint8_t b, uint8_t tag) { return b == static_cast<uint8_t>((tag << 4) | 0x0F); }

bool decode_tag_len(const uint8_t* buf, size_t len, size_t* pos, uint8_t* tag, bool* context,
                    bool* open, bool* close, size_t* value_len) {
    if (*pos >= len) return false;
    uint8_t b = buf[(*pos)++];
    *tag = b >> 4;
    *context = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    *open = *context && lvt == 6;
    *close = *context && lvt == 7;
    if (*open || *close) {
        *value_len = 0;
        return true;
    }
    if (lvt == 5) {
        if (*pos >= len) return false;
        *value_len = buf[(*pos)++];
    } else {
        *value_len = lvt;
    }
    return *pos + *value_len <= len;
}

bool decode_app_value(const uint8_t* buf, size_t len, size_t* pos, Value* out) {
    uint8_t tag = 0;
    bool context = false, open = false, close = false;
    size_t n = 0;
    if (!decode_tag_len(buf, len, pos, &tag, &context, &open, &close, &n) || context || open ||
        close) {
        return false;
    }
    const uint8_t* p = buf + *pos;
    *pos += n;
    *out = {};
    switch (tag) {
        case 0:
            out->type = ValueType::Null;
            return n == 0;
        case 1:
            out->type = ValueType::Bool;
            out->b = n != 0;
            return true;
        case 2:
            if (n == 0 || n > 4) return false;
            out->type = ValueType::Unsigned;
            out->u = read_be(p, n);
            return true;
        case 4: {
            if (n != 4) return false;
            uint32_t bits = read_be(p, 4);
            out->type = ValueType::Real;
            memcpy(&out->real, &bits, sizeof(out->real));
            return true;
        }
        case 7: {
            if (n < 1) return false;
            out->type = ValueType::String;
            size_t copy = n - 1;
            if (copy >= sizeof(out->str)) copy = sizeof(out->str) - 1;
            memcpy(out->str, p + 1, copy);
            out->str[copy] = '\0';
            return p[0] == 0;  // ANSI/UTF-8 compatible single-byte encoding
        }
        case 8:
            if (n < 1) return false;
            out->type = ValueType::BitString;
            out->bit_count = static_cast<uint8_t>((n - 1) * 8 - p[0]);
            out->bits = n > 1 ? p[1] : 0;
            return true;
        case 9:
            if (n == 0 || n > 4) return false;
            out->type = ValueType::Enumerated;
            out->u = read_be(p, n);
            return true;
        case 12: {
            if (n != 4) return false;
            uint32_t oid = read_be(p, 4);
            out->type = ValueType::ObjectId;
            out->object.type = static_cast<ObjectType>(oid >> 22);
            out->object.instance = oid & 0x3FFFFF;
            return true;
        }
    }
    return false;
}

bool read_context_uint(const uint8_t* buf, size_t len, size_t* pos, uint8_t wanted, uint32_t* out) {
    uint8_t tag = 0;
    bool context = false, open = false, close = false;
    size_t n = 0;
    if (!decode_tag_len(buf, len, pos, &tag, &context, &open, &close, &n) || !context || open ||
        close || tag != wanted || n == 0 || n > 4) {
        return false;
    }
    *out = read_be(buf + *pos, n);
    *pos += n;
    return true;
}

bool read_context_object(const uint8_t* buf, size_t len, size_t* pos, uint8_t wanted,
                         ObjectId* out) {
    uint32_t v = 0;
    if (!read_context_uint(buf, len, pos, wanted, &v)) return false;
    *out = {static_cast<ObjectType>(v >> 22), v & 0x3FFFFF};
    return true;
}

bool read_property_result(const uint8_t* buf, size_t len, size_t* pos, ObjectId obj,
                          PropertyValue* pv) {
    uint32_t prop = 0;
    if (!read_context_uint(buf, len, pos, 2, &prop)) return false;
    pv->object = obj;
    pv->property = prop;
    if (*pos >= len) return false;
    if (is_open(buf[*pos], 5)) {
        ++*pos;
        uint32_t cls = 0, code = 0;
        if (!read_context_uint(buf, len, pos, 0, &cls) || !read_context_uint(buf, len, pos, 1, &code) ||
            *pos >= len || !is_close(buf[*pos], 5)) {
            return false;
        }
        ++*pos;
        pv->error = true;
        pv->error_info.error_class = static_cast<uint8_t>(cls);
        pv->error_info.error_code = static_cast<uint8_t>(code);
        return true;
    }
    if (!is_open(buf[*pos], 4)) return false;
    ++*pos;
    if (!decode_app_value(buf, len, pos, &pv->value)) return false;
    if (*pos >= len || !is_close(buf[*pos], 4)) return false;
    ++*pos;
    return true;
}

}  // namespace

bool Writer::u8(uint8_t v) {
    if (pos_ >= cap_) {
        ok_ = false;
        return false;
    }
    buf_[pos_++] = v;
    return true;
}

bool Writer::bytes(const uint8_t* data, size_t len) {
    if (pos_ + len > cap_) {
        ok_ = false;
        return false;
    }
    memcpy(buf_ + pos_, data, len);
    pos_ += len;
    return true;
}

bool Writer::tag(uint8_t tag_id, size_t len) {
    if (len < 5) return u8(static_cast<uint8_t>((tag_id << 4) | len));
    return u8(static_cast<uint8_t>((tag_id << 4) | 5)) && u8(static_cast<uint8_t>(len));
}

bool Writer::application_object_id(ObjectType type, uint32_t instance) {
    uint32_t oid = object_id_word(type, instance);
    uint8_t b[4] = {static_cast<uint8_t>(oid >> 24), static_cast<uint8_t>(oid >> 16),
                    static_cast<uint8_t>(oid >> 8), static_cast<uint8_t>(oid)};
    return tag(12, 4) && bytes(b, 4);
}

bool Writer::application_unsigned(uint32_t v) {
    uint8_t b[4];
    size_t n = 1;
    if (v <= 0xFF) {
        b[0] = static_cast<uint8_t>(v);
    } else if (v <= 0xFFFF) {
        n = 2;
        b[0] = static_cast<uint8_t>(v >> 8);
        b[1] = static_cast<uint8_t>(v);
    } else {
        n = 4;
        b[0] = static_cast<uint8_t>(v >> 24);
        b[1] = static_cast<uint8_t>(v >> 16);
        b[2] = static_cast<uint8_t>(v >> 8);
        b[3] = static_cast<uint8_t>(v);
    }
    return tag(2, n) && bytes(b, n);
}

bool Writer::application_enumerated(uint32_t v) {
    uint8_t b[4];
    size_t n = v <= 0xFF ? 1 : (v <= 0xFFFF ? 2 : 4);
    for (size_t i = 0; i < n; ++i) b[n - 1 - i] = static_cast<uint8_t>(v >> (8 * i));
    return tag(9, n) && bytes(b, n);
}

bool Writer::application_real(float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t b[4] = {static_cast<uint8_t>(bits >> 24), static_cast<uint8_t>(bits >> 16),
                    static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)};
    return tag(4, 4) && bytes(b, 4);
}

bool Writer::application_string(const char* s) {
    size_t n = strlen(s);
    if (n > 63) n = 63;
    return tag(7, n + 1) && u8(0) && bytes(reinterpret_cast<const uint8_t*>(s), n);
}

bool Writer::application_bitstring(uint8_t bits, uint8_t bit_count) {
    uint8_t b[2] = {static_cast<uint8_t>(8 - bit_count), bits};
    return tag(8, 2) && bytes(b, 2);
}

uint32_t object_id_word(ObjectType type, uint32_t instance) {
    return (static_cast<uint32_t>(type) << 22) | (instance & 0x3FFFFF);
}

bool build_read_property(uint8_t invoke, ObjectType type, uint32_t instance, uint32_t property,
                         uint32_t array_index, bool has_array_index, uint8_t* out, size_t cap,
                         size_t* out_len) {
    size_t pos = 0;
    if (!begin(invoke, kServiceReadProperty, out, cap, &pos)) return false;
    if (!context_object_id(out, cap, &pos, 0, type, instance)) return false;
    if (!context_tag(out, cap, &pos, 1, property)) return false;
    if (has_array_index && !context_tag(out, cap, &pos, 2, array_index)) return false;
    return finish(out, pos, out_len);
}

bool build_read_property_multiple(uint8_t invoke, ObjectType type, uint32_t instance,
                                  const uint32_t* properties, size_t property_count,
                                  uint8_t* out, size_t cap, size_t* out_len) {
    size_t pos = 0;
    if (!begin(invoke, kServiceReadPropertyMultiple, out, cap, &pos)) return false;
    if (!context_object_id(out, cap, &pos, 0, type, instance)) return false;
    if (!opening(out, cap, &pos, 1)) return false;
    for (size_t i = 0; i < property_count; ++i) {
        if (!context_tag(out, cap, &pos, 0, properties[i])) return false;
    }
    if (!closing(out, cap, &pos, 1)) return false;
    return finish(out, pos, out_len);
}

bool build_who_is(uint8_t* out, size_t cap, size_t* out_len) {
    if (cap < 8 || !out || !out_len) return false;
    out[0] = kBvlcType;
    out[1] = kBvlcOriginalBroadcast;
    out[2] = 0;
    out[3] = 8;
    out[4] = kNpduVersion;
    out[5] = 0;
    out[6] = 0x10;  // unconfirmed request
    out[7] = kServiceWhoIs;
    *out_len = 8;
    return true;
}

static bool read_app_uint_tag(const uint8_t* buf, size_t total, size_t* pos, uint8_t tag,
                              uint32_t* out) {
    if (*pos >= total || (buf[*pos] >> 4) != tag) return false;
    size_t n = buf[(*pos)++] & 0x07;
    if (n == 5) {
        if (*pos >= total) return false;
        n = buf[(*pos)++];
    }
    if (n == 0 || n > 4 || *pos + n > total) return false;
    *out = read_be(buf + *pos, n);
    *pos += n;
    return true;
}

Parse parse_i_am(const uint8_t* buf, size_t len, IAm* out) {
    if (!buf || !out || len < 12) return Parse::Incomplete;
    if (buf[0] != kBvlcType ||
        (buf[1] != kBvlcOriginalUnicast && buf[1] != kBvlcOriginalBroadcast)) {
        return Parse::BadFrame;
    }
    uint16_t total = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    if (total > len) return Parse::Incomplete;
    if (total < 12 || buf[4] != kNpduVersion || (buf[5] & 0x28) != 0) return Parse::BadFrame;
    if ((buf[6] & 0xF0) != 0x10 || buf[7] != kServiceIAm) return Parse::WrongService;
    size_t pos = 8;
    uint32_t obj = 0;
    if (!read_app_uint_tag(buf, total, &pos, 12, &obj)) return Parse::BadFrame;
    if ((obj >> 22) != static_cast<uint32_t>(ObjectType::Device)) return Parse::BadFrame;
    IAm iam{};
    iam.device_instance = obj & 0x3FFFFF;
    if (!read_app_uint_tag(buf, total, &pos, 2, &iam.max_apdu) ||
        !read_app_uint_tag(buf, total, &pos, 9, &iam.segmentation) ||
        !read_app_uint_tag(buf, total, &pos, 2, &iam.vendor_id)) {
        return Parse::BadFrame;
    }
    *out = iam;
    return Parse::Ok;
}

Parse parse_read_property_ack(const uint8_t* buf, size_t len, uint8_t invoke, uint32_t property,
                              Value* out, ErrorInfo* err, const ObjectId* expected_object) {
    Header h;
    Parse p = parse_header(buf, len, invoke, kServiceReadProperty, &h, err);
    if (p != Parse::Ok) return p;
    size_t total = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    size_t pos = h.pos;
    ObjectId oid;
    uint32_t prop = 0;
    if (!read_context_object(buf, total, &pos, 0, &oid) ||
        !read_context_uint(buf, total, &pos, 1, &prop)) {
        return Parse::BadFrame;
    }
    if (expected_object &&
        (oid.type != expected_object->type ||
         (expected_object->instance != kDeviceWildcard &&
          oid.instance != expected_object->instance))) {
        return Parse::BadFrame;
    }
    if (prop != property) return Parse::WrongService;
    if (pos < total && (buf[pos] >> 4) == 2 && (buf[pos] & 0x08)) {
        uint32_t ignored = 0;
        if (!read_context_uint(buf, total, &pos, 2, &ignored)) return Parse::BadFrame;
    }
    if (pos >= total || !is_open(buf[pos], 3)) return Parse::BadFrame;
    ++pos;
    if (!decode_app_value(buf, total, &pos, out)) return Parse::BadFrame;
    if (pos >= total || !is_close(buf[pos], 3)) return Parse::BadFrame;
    return Parse::Ok;
}

Parse parse_read_property_multiple_ack(const uint8_t* buf, size_t len, uint8_t invoke,
                                       PropertyValue* out, size_t cap, size_t* count,
                                       ErrorInfo* err, const ObjectId* expected_object) {
    *count = 0;
    Header h;
    Parse p = parse_header(buf, len, invoke, kServiceReadPropertyMultiple, &h, err);
    if (p != Parse::Ok) return p;
    size_t total = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    size_t pos = h.pos;
    while (pos < total) {
        ObjectId obj;
        if (!read_context_object(buf, total, &pos, 0, &obj) || pos >= total || !is_open(buf[pos], 1)) {
            return Parse::BadFrame;
        }
        if (expected_object &&
            (obj.type != expected_object->type ||
             (expected_object->instance != kDeviceWildcard &&
              obj.instance != expected_object->instance))) {
            return Parse::BadFrame;
        }
        ++pos;
        while (pos < total && !is_close(buf[pos], 1)) {
            if (*count >= cap) return Parse::Unsupported;
            if (!read_property_result(buf, total, &pos, obj, &out[*count])) return Parse::BadFrame;
            ++*count;
        }
        if (pos >= total || !is_close(buf[pos], 1)) return Parse::BadFrame;
        ++pos;
    }
    return Parse::Ok;
}

bool frame_matches(const uint8_t* buf, size_t len, uint8_t invoke, uint8_t service) {
    Header h;
    ErrorInfo err;
    Parse p = parse_header(buf, len, invoke, service, &h, &err);
    return p == Parse::Ok || p == Parse::Error || p == Parse::Reject || p == Parse::Abort;
}

bool frame_matches_object(const uint8_t* buf, size_t len, uint8_t invoke, uint8_t service,
                          const ObjectId& expected) {
    Header h;
    ErrorInfo err;
    Parse p = parse_header(buf, len, invoke, service, &h, &err);
    if (p == Parse::Error || p == Parse::Reject || p == Parse::Abort) return true;
    if (p != Parse::Ok) return false;
    size_t total = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    size_t pos = h.pos;
    ObjectId oid;
    if (!read_context_object(buf, total, &pos, 0, &oid)) return false;
    return oid.type == expected.type &&
           (expected.instance == kDeviceWildcard || oid.instance == expected.instance);
}

const char* parse_name(Parse p) {
    switch (p) {
        case Parse::Ok: return "ok";
        case Parse::Incomplete: return "incomplete";
        case Parse::BadFrame: return "bad_frame";
        case Parse::WrongInvoke: return "wrong_invoke";
        case Parse::WrongService: return "wrong_service";
        case Parse::Error: return "error";
        case Parse::Reject: return "reject";
        case Parse::Abort: return "abort";
        case Parse::Unsupported: return "unsupported";
        case Parse::NotFound: return "not_found";
    }
    return "unknown";
}

bool object_type_from_perf(uint8_t perf_type, ObjectType* out) {
    if (perf_type == 0) {
        *out = ObjectType::AnalogInput;
        return true;
    }
    if (perf_type == 1) {
        *out = ObjectType::AnalogValue;
        return true;
    }
    return false;
}

}  // namespace bacnet
