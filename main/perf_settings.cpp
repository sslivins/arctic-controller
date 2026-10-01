#include "perf_settings.h"

#include <esp_crc.h>
#include <esp_log.h>
#include <nvs.h>

#include <math.h>
#include <string.h>

namespace perf {

namespace {

const char* TAG = "perf_settings";
const char* kNvsNamespace = "perf";
const char* kNvsKey = "cfg";
constexpr uint8_t kBlobVersion = 1;

void put_u16(uint8_t*& p, uint16_t v) {
    *p++ = static_cast<uint8_t>(v >> 8);
    *p++ = static_cast<uint8_t>(v & 0xFF);
}
uint16_t get_u16(const uint8_t*& p) {
    uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);
    p += 2;
    return v;
}

struct NameMap {
    uint8_t value;
    const char* name;
};

template <size_t N>
const char* lookup_name(const NameMap (&map)[N], uint8_t v) {
    for (const auto& m : map) {
        if (m.value == v) return m.name;
    }
    return "unknown";
}

template <size_t N>
bool lookup_value(const NameMap (&map)[N], const char* s, uint8_t* out) {
    if (!s) return false;
    for (const auto& m : map) {
        if (strcmp(m.name, s) == 0) {
            *out = m.value;
            return true;
        }
    }
    return false;
}

const NameMap kSources[] = {
    {static_cast<uint8_t>(SensorSource::HeatPump), "heat_pump"},
    {static_cast<uint8_t>(SensorSource::ModbusTcp), "modbus_tcp"},
};
const NameMap kFluids[] = {
    {static_cast<uint8_t>(arctic::LoopFluid::Water), "water"},
    {static_cast<uint8_t>(arctic::LoopFluid::PropyleneGlycol), "propylene_glycol"},
    {static_cast<uint8_t>(arctic::LoopFluid::EthyleneGlycol), "ethylene_glycol"},
};
const NameMap kRegisterTypes[] = {
    {static_cast<uint8_t>(RegisterType::Input), "input"},
    {static_cast<uint8_t>(RegisterType::Holding), "holding"},
};
const NameMap kValueTypes[] = {
    {static_cast<uint8_t>(ValueType::Int16), "int16"},
    {static_cast<uint8_t>(ValueType::Uint16), "uint16"},
    {static_cast<uint8_t>(ValueType::Float32), "float32"},
    {static_cast<uint8_t>(ValueType::Float32Swapped), "float32_swapped"},
};
const NameMap kNoReadings[] = {
    {static_cast<uint8_t>(NoReading::None), "none"},
    {static_cast<uint8_t>(NoReading::X8000), "0x8000"},
    {static_cast<uint8_t>(NoReading::X7FFF), "0x7fff"},
    {static_cast<uint8_t>(NoReading::XFFFF), "0xffff"},
};

template <typename E, size_t N>
bool parse_enum(const NameMap (&map)[N], const char* s, E* out) {
    uint8_t v = 0;
    if (!lookup_value(map, s, &v)) return false;
    *out = static_cast<E>(v);
    return true;
}

}  // namespace

SensorConfig default_sensor() {
    SensorConfig s{};
    s.source = SensorSource::HeatPump;
    s.host[0] = '\0';
    s.port = kDefaultPort;
    s.unit_id = kDefaultUnitId;
    s.address = 0;
    s.reg_type = RegisterType::Input;
    s.value_type = ValueType::Int16;
    s.scale_exp = -2;
    s.no_reading = NoReading::X8000;
    s.rom_known = false;
    return s;
}

Settings defaults() {
    Settings s{};
    s.flow_lpm_x10 = kFlowDefaultX10;
    s.fluid = arctic::LoopFluid::Water;
    s.glycol_pct = kGlycolDefault;
    for (auto& sensor : s.sensors) sensor = default_sensor();
    return s;
}

bool host_valid(const char* host) {
    if (!host || !host[0]) return false;
    size_t len = strnlen(host, kHostMax);
    if (len >= kHostMax) return false;
    if (host[0] == '.' || host[0] == '-' || host[len - 1] == '.' || host[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = host[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) return false;
        if (c == '.' && i + 1 < len && host[i + 1] == '.') return false;
    }
    return true;
}

uint16_t register_count(ValueType t) {
    return (t == ValueType::Float32 || t == ValueType::Float32Swapped) ? 2 : 1;
}

Invalid validate_sensor(const SensorConfig& s) {
    if (s.source != SensorSource::HeatPump && s.source != SensorSource::ModbusTcp) {
        return Invalid::Source;
    }
    // The Modbus fields are kept while the heat pump's own sensor is selected,
    // so switching back restores them; they must still be well-formed.
    size_t len = strnlen(s.host, kHostMax);
    if (len >= kHostMax) return Invalid::Host;
    if (s.source == SensorSource::ModbusTcp || len > 0) {
        if (!host_valid(s.host)) return Invalid::Host;
    }
    if (s.port == 0) return Invalid::Port;
    if (static_cast<uint8_t>(s.reg_type) > static_cast<uint8_t>(RegisterType::Holding)) {
        return Invalid::RegisterType;
    }
    if (static_cast<uint8_t>(s.value_type) > static_cast<uint8_t>(ValueType::Float32Swapped)) {
        return Invalid::ValueType;
    }
    if (static_cast<uint32_t>(s.address) + register_count(s.value_type) - 1 > 0xFFFF) {
        return Invalid::Register;
    }
    if (s.scale_exp > 0 || s.scale_exp < -3) return Invalid::Scale;
    if (static_cast<uint8_t>(s.no_reading) > static_cast<uint8_t>(NoReading::XFFFF)) {
        return Invalid::NoReading;
    }
    return Invalid::None;
}

Invalid validate(const Settings& s) {
    if (s.flow_lpm_x10 < kFlowMinX10 || s.flow_lpm_x10 > kFlowMaxX10) return Invalid::Flow;
    if (static_cast<uint8_t>(s.fluid) > static_cast<uint8_t>(arctic::LoopFluid::EthyleneGlycol)) {
        return Invalid::Fluid;
    }
    if (s.glycol_pct > arctic::kGlycolPctMax || s.glycol_pct % kGlycolStep != 0) {
        return Invalid::Glycol;
    }
    for (const auto& sensor : s.sensors) {
        Invalid v = validate_sensor(sensor);
        if (v != Invalid::None) return v;
    }
    return Invalid::None;
}

const char* invalid_name(Invalid v) {
    switch (v) {
        case Invalid::None: return "none";
        case Invalid::Flow: return "flow_lpm";
        case Invalid::Fluid: return "fluid";
        case Invalid::Glycol: return "glycol_pct";
        case Invalid::Source: return "source";
        case Invalid::Host: return "host";
        case Invalid::Port: return "port";
        case Invalid::Register: return "register";
        case Invalid::RegisterType: return "register_type";
        case Invalid::ValueType: return "value_type";
        case Invalid::Scale: return "scale";
        case Invalid::NoReading: return "no_reading";
    }
    return "unknown";
}

bool same_source(const SensorConfig& a, const SensorConfig& b) {
    return a.source == b.source && strncmp(a.host, b.host, kHostMax) == 0 && a.port == b.port &&
           a.unit_id == b.unit_id && a.address == b.address && a.reg_type == b.reg_type &&
           a.value_type == b.value_type;
}

DecodeResult decode_value(const SensorConfig& s, const uint16_t* regs, float* out_c) {
    float raw = 0.0f;
    switch (s.value_type) {
        case ValueType::Int16:
        case ValueType::Uint16: {
            uint16_t r = regs[0];
            bool missing = (s.no_reading == NoReading::X8000 && r == 0x8000) ||
                           (s.no_reading == NoReading::X7FFF && r == 0x7FFF) ||
                           (s.no_reading == NoReading::XFFFF && r == 0xFFFF);
            if (missing) return DecodeResult::NoReading;
            raw = (s.value_type == ValueType::Int16) ? static_cast<float>(static_cast<int16_t>(r))
                                                     : static_cast<float>(r);
            break;
        }
        case ValueType::Float32:
        case ValueType::Float32Swapped: {
            uint16_t hi = s.value_type == ValueType::Float32 ? regs[0] : regs[1];
            uint16_t lo = s.value_type == ValueType::Float32 ? regs[1] : regs[0];
            uint32_t bits = (static_cast<uint32_t>(hi) << 16) | lo;
            memcpy(&raw, &bits, sizeof(raw));
            if (!isfinite(raw)) return DecodeResult::NoReading;
            break;
        }
        default:
            return DecodeResult::NoReading;
    }
    float v = raw;
    for (int8_t e = s.scale_exp; e < 0; ++e) v /= 10.0f;
    if (!(v >= kMinPlausibleC && v <= kMaxPlausibleC)) return DecodeResult::OutOfRange;
    *out_c = v;
    return DecodeResult::Ok;
}

void serialize(const Settings& s, uint8_t* buf) {
    uint8_t* p = buf;
    *p++ = kBlobVersion;
    put_u16(p, s.flow_lpm_x10);
    *p++ = static_cast<uint8_t>(s.fluid);
    *p++ = s.glycol_pct;
    for (const auto& sensor : s.sensors) {
        *p++ = static_cast<uint8_t>(sensor.source);
        memset(p, 0, kHostMax);
        strncpy(reinterpret_cast<char*>(p), sensor.host, kHostMax - 1);
        p += kHostMax;
        put_u16(p, sensor.port);
        *p++ = sensor.unit_id;
        put_u16(p, sensor.address);
        *p++ = static_cast<uint8_t>(sensor.reg_type);
        *p++ = static_cast<uint8_t>(sensor.value_type);
        *p++ = static_cast<uint8_t>(sensor.scale_exp);
        *p++ = static_cast<uint8_t>(sensor.no_reading);
        *p++ = sensor.rom_known ? 1 : 0;
        memcpy(p, sensor.rom, kRomLen);
        p += kRomLen;
    }
    uint32_t crc = esp_crc32_le(0, buf, static_cast<uint32_t>(p - buf));
    memcpy(p, &crc, sizeof(crc));
}

bool deserialize(const uint8_t* buf, size_t len, Settings* out) {
    if (!buf || len != kBlobSize || buf[0] != kBlobVersion) return false;
    uint32_t stored = 0;
    memcpy(&stored, buf + kBlobSize - 4, sizeof(stored));
    if (esp_crc32_le(0, buf, kBlobSize - 4) != stored) return false;

    Settings s{};
    const uint8_t* p = buf + 1;
    s.flow_lpm_x10 = get_u16(p);
    s.fluid = static_cast<arctic::LoopFluid>(*p++);
    s.glycol_pct = *p++;
    for (auto& sensor : s.sensors) {
        sensor.source = static_cast<SensorSource>(*p++);
        memcpy(sensor.host, p, kHostMax);
        sensor.host[kHostMax - 1] = '\0';
        p += kHostMax;
        sensor.port = get_u16(p);
        sensor.unit_id = *p++;
        sensor.address = get_u16(p);
        sensor.reg_type = static_cast<RegisterType>(*p++);
        sensor.value_type = static_cast<ValueType>(*p++);
        sensor.scale_exp = static_cast<int8_t>(*p++);
        sensor.no_reading = static_cast<NoReading>(*p++);
        sensor.rom_known = *p++ != 0;
        memcpy(sensor.rom, p, kRomLen);
        p += kRomLen;
    }
    if (validate(s) != Invalid::None) return false;
    *out = s;
    return true;
}

Settings load() {
    Settings s = defaults();
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return s;
    uint8_t buf[kBlobSize];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(h, kNvsKey, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) return s;
    if (!deserialize(buf, len, &s)) {
        ESP_LOGW(TAG, "Stored heat output settings unreadable; using defaults");
        return defaults();
    }
    return s;
}

bool save(const Settings& s) {
    if (validate(s) != Invalid::None) return false;
    uint8_t buf[kBlobSize];
    serialize(s, buf);
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, kNvsKey, buf, sizeof(buf));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving heat output settings failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool uses_network(const Settings& s) {
    for (const auto& sensor : s.sensors) {
        if (sensor.source == SensorSource::ModbusTcp) return true;
    }
    return false;
}

const char* source_name(SensorSource s) { return lookup_name(kSources, static_cast<uint8_t>(s)); }
const char* fluid_name(arctic::LoopFluid f) { return lookup_name(kFluids, static_cast<uint8_t>(f)); }
const char* register_type_name(RegisterType t) {
    return lookup_name(kRegisterTypes, static_cast<uint8_t>(t));
}
const char* value_type_name(ValueType t) { return lookup_name(kValueTypes, static_cast<uint8_t>(t)); }
const char* no_reading_name(NoReading n) { return lookup_name(kNoReadings, static_cast<uint8_t>(n)); }
bool parse_source(const char* s, SensorSource* out) { return parse_enum(kSources, s, out); }
bool parse_fluid(const char* s, arctic::LoopFluid* out) { return parse_enum(kFluids, s, out); }
bool parse_register_type(const char* s, RegisterType* out) {
    return parse_enum(kRegisterTypes, s, out);
}
bool parse_value_type(const char* s, ValueType* out) { return parse_enum(kValueTypes, s, out); }
bool parse_no_reading(const char* s, NoReading* out) { return parse_enum(kNoReadings, s, out); }

}  // namespace perf
