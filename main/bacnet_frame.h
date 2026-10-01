/*
 * Small BACnet/IP client codec for COP temperature sensors.
 *
 * Pure byte building/parsing; sockets live in ext_temp_sensors.cpp. Supports
 * direct unicast ReadProperty and ReadPropertyMultiple for a bounded subset of
 * application tags used by Thermux Analog Input/Value objects.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bacnet {

constexpr uint16_t kDefaultPort = 47808;
constexpr uint32_t kInstanceMax = 4194302;
constexpr uint32_t kDeviceWildcard = 4194303;
constexpr size_t kMaxApdu = 1476;
constexpr size_t kMaxFrame = 1500;

enum class ObjectType : uint16_t {
    AnalogInput = 0,
    AnalogValue = 2,
    Device = 8,
};

enum Property : uint32_t {
    PROP_OBJECT_IDENTIFIER = 75,
    PROP_OBJECT_LIST = 76,
    PROP_OBJECT_NAME = 77,
    PROP_PRESENT_VALUE = 85,
    PROP_RELIABILITY = 103,
    PROP_STATUS_FLAGS = 111,
    PROP_UNITS = 117,
    PROP_DESCRIPTION = 28,
    PROP_MODEL_NAME = 70,
};

enum class ValueType : uint8_t {
    Null,
    Bool,
    Unsigned,
    Enumerated,
    Real,
    String,
    BitString,
    ObjectId,
};

struct ObjectId {
    ObjectType type;
    uint32_t instance;
};

struct Value {
    ValueType type = ValueType::Null;
    uint32_t u = 0;
    float real = 0.0f;
    bool b = false;
    char str[64] = {};
    uint8_t bit_count = 0;
    uint8_t bits = 0;
    ObjectId object{};
};

enum class Parse : uint8_t {
    Ok,
    Incomplete,
    BadFrame,
    WrongInvoke,
    WrongService,
    Error,
    Reject,
    Abort,
    Unsupported,
    NotFound,
};

struct ErrorInfo {
    uint8_t error_class = 0;
    uint8_t error_code = 0;
    uint8_t reason = 0;
};

struct PropertyValue {
    ObjectId object{};
    uint32_t property = 0;
    bool error = false;
    ErrorInfo error_info{};
    Value value{};
};

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
    bool ok() const { return ok_; }
    size_t size() const { return pos_; }
    bool u8(uint8_t v);
    bool bytes(const uint8_t* data, size_t len);
    bool application_object_id(ObjectType type, uint32_t instance);
    bool application_unsigned(uint32_t v);
    bool application_enumerated(uint32_t v);
    bool application_real(float v);
    bool application_string(const char* s);
    bool application_bitstring(uint8_t bits, uint8_t bit_count);

private:
    bool tag(uint8_t tag, size_t len);
    uint8_t* buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool ok_ = true;
};

bool build_read_property(uint8_t invoke, ObjectType type, uint32_t instance, uint32_t property,
                         uint32_t array_index, bool has_array_index, uint8_t* out, size_t cap,
                         size_t* out_len);

bool build_read_property_multiple(uint8_t invoke, ObjectType type, uint32_t instance,
                                  const uint32_t* properties, size_t property_count,
                                  uint8_t* out, size_t cap, size_t* out_len);

Parse parse_read_property_ack(const uint8_t* buf, size_t len, uint8_t invoke, uint32_t property,
                              Value* out, ErrorInfo* err, const ObjectId* expected_object = nullptr);

Parse parse_read_property_multiple_ack(const uint8_t* buf, size_t len, uint8_t invoke,
                                       PropertyValue* out, size_t cap, size_t* count,
                                       ErrorInfo* err, const ObjectId* expected_object = nullptr);

bool frame_matches(const uint8_t* buf, size_t len, uint8_t invoke, uint8_t service);
bool frame_matches_object(const uint8_t* buf, size_t len, uint8_t invoke, uint8_t service,
                          const ObjectId& expected);

const char* parse_name(Parse p);
uint32_t object_id_word(ObjectType type, uint32_t instance);
bool object_type_from_perf(uint8_t perf_type, ObjectType* out);

}  // namespace bacnet
