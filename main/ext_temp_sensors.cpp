#include "ext_temp_sensors.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "bacnet_frame.h"
#include "mbtcp_frame.h"
#include "wifi_manager.h"

namespace ext_temp {

bool run_browse_internal(const char* host, uint16_t port, BrowseResult* out, uint32_t timeout_ms);

namespace {

const char* TAG = "ext_temp";

constexpr uint32_t kPollMs = 15000;
constexpr uint32_t kIoTimeoutMs = 1500;
constexpr uint32_t kBrowseMaxMs = 8000;
constexpr uint32_t kBrowseMaxObjects = 512;
constexpr size_t kBrowseMaxResults = 64;
// BACnet ReadPropertyMultiple parsing keeps a 1500-byte datagram plus a small
// property list on the worker stack. The task uses PSRAM, so this modest bump
// avoids tight-stack failures without increasing internal-RAM pressure.
constexpr uint32_t kWorkerStack = 6144;
constexpr uint8_t kBacnetUnitsC = 62;
constexpr uint8_t kBacnetUnitsF = 64;
constexpr uint8_t kBacnetUnitsK = 63;
constexpr uint8_t kBacnetReliabilityNoFault = 0;
constexpr uint32_t kBacnetReliabilityUnknown = 0xFFFFFFFFu;

void log_stack_watermark(const char* phase) {
    ESP_LOGI(TAG, "external sensor worker stack high-water after %s: %u words",
             phase, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

enum class ThermuxState : uint8_t { Unknown, Yes, No };

struct Slot {
    SlotStatus status;
    uint32_t last_ok_ms;
    ThermuxState thermux;
};

SemaphoreHandle_t s_mutex = nullptr;
// Serializes persisting settings (apply_settings and service) so an older
// snapshot can never be written after a newer one.
SemaphoreHandle_t s_save_mutex = nullptr;
TaskHandle_t s_task = nullptr;
bool s_worker_allowed = false;
bool s_save_pending = false;  // learned ROM ID awaiting service(); guarded by s_mutex
perf::Settings s_settings;
perf::SourceSelector s_selector;
Slot s_slots[perf::kSlotCount];
uint16_t s_transaction = 0;
uint8_t s_bacnet_invoke = 0;

// Test request/response, guarded by s_mutex.
bool s_test_pending = false;
uint32_t s_test_ticket = 0;
uint32_t s_test_done_ticket = 0;
perf::SensorConfig s_test_cfg;
TestResult s_test_result;
bool s_browse_busy = false;
bool s_browse_pending = false;
uint32_t s_browse_ticket = 0;
uint32_t s_browse_done_ticket = 0;
char s_browse_host[perf::kHostMax] = {};
uint16_t s_browse_port = 0;
BrowseResult* s_browse_out = nullptr;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The worker starts before Wi-Fi (and so before lwIP's tcpip thread) exists; any
// socket or DNS call before then asserts inside lwIP. Only touch the network
// once the station has an address.
bool network_up() {
    char ip[16];
    return wifi_mgr_get_ip_addr(ip, sizeof(ip));
}

void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mutex); }

uint8_t function_code(perf::RegisterType t) {
    return t == perf::RegisterType::Holding ? mbtcp::kFcReadHolding : mbtcp::kFcReadInput;
}

class Connection {
public:
    ~Connection() { close(); }

    Error open(const char* host, uint16_t port) {
        close();
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port_s[6];
        snprintf(port_s, sizeof(port_s), "%u", port);
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host, port_s, &hints, &res) != 0 || res == nullptr) {
            if (res) freeaddrinfo(res);
            return Error::Resolve;
        }
        fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0) {
            freeaddrinfo(res);
            return Error::Connect;
        }
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        int r = connect(fd_, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        if (r != 0) {
            if (errno != EINPROGRESS) {
                close();
                return Error::Connect;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd_, &wfds);
            struct timeval tv = {kIoTimeoutMs / 1000, (kIoTimeoutMs % 1000) * 1000};
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (select(fd_ + 1, nullptr, &wfds, nullptr, &tv) <= 0 ||
                getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
                close();
                return Error::Connect;
            }
        }
        fcntl(fd_, F_SETFL, flags);
        struct timeval tv = {kIoTimeoutMs / 1000, (kIoTimeoutMs % 1000) * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        snprintf(host_, sizeof(host_), "%s", host);
        port_ = port;
        return Error::None;
    }

    bool is_open_to(const char* host, uint16_t port) const {
        return fd_ >= 0 && port_ == port && strcmp(host_, host) == 0;
    }

    Error read(uint8_t unit, uint8_t fc, uint16_t start, uint16_t count, uint16_t* regs,
               uint8_t* exception) {
        if (fd_ < 0) return Error::Connect;
        uint16_t tid = ++s_transaction;
        uint8_t req[mbtcp::kRequestLen];
        if (!mbtcp::build_read(tid, unit, fc, start, count, req)) return Error::Protocol;
        if (send(fd_, req, sizeof(req), 0) != static_cast<int>(sizeof(req))) {
            close();
            return Error::Timeout;
        }
        uint8_t buf[mbtcp::kMaxResponseLen];
        size_t got = 0;
        size_t need = 6;
        uint32_t start_ms = now_ms();
        while (got < need) {
            int n = recv(fd_, buf + got, need - got, 0);
            if (n <= 0 || now_ms() - start_ms > 2 * kIoTimeoutMs) {
                close();
                return Error::Timeout;
            }
            got += static_cast<size_t>(n);
            if (need == 6 && got >= 6) {
                size_t total = mbtcp::frame_length(buf, got);
                if (total == 0) {
                    close();
                    return Error::Protocol;
                }
                need = total;
            }
        }
        mbtcp::Parse p = mbtcp::parse_read(buf, got, tid, unit, fc, count, regs, exception);
        if (p == mbtcp::Parse::Ok) return Error::None;
        if (p == mbtcp::Parse::Exception) return Error::Exception;
        ESP_LOGD(TAG, "Bad reply: %s", mbtcp::parse_name(p));
        close();
        return Error::Protocol;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    char host_[sizeof(perf::SensorConfig::host)] = {};
    uint16_t port_ = 0;
};

struct ValueRead {
    Error error;
    uint8_t exception;
    float celsius;
};

ValueRead read_value(Connection& c, const perf::SensorConfig& cfg) {
    ValueRead r = {Error::None, 0, 0.0f};
    uint16_t regs[2] = {};
    r.error = c.read(cfg.unit_id, function_code(cfg.reg_type), cfg.address,
                     perf::register_count(cfg.value_type), regs, &r.exception);
    if (r.error != Error::None) return r;
    switch (perf::decode_value(cfg, regs, &r.celsius)) {
        case perf::DecodeResult::Ok: break;
        case perf::DecodeResult::NoReading: r.error = Error::NoReading; break;
        case perf::DecodeResult::OutOfRange: r.error = Error::OutOfRange; break;
    }
    return r;
}

bool probe_thermux(Connection& c, const perf::SensorConfig& cfg) {
    uint16_t info[perf::thermux::kInfoCount];
    uint8_t exc = 0;
    return c.read(cfg.unit_id, mbtcp::kFcReadInput, 0, perf::thermux::kInfoCount, info, &exc) ==
               Error::None &&
           perf::thermux::info_matches(info);
}

// False when the read failed or the channel has no sensor assigned.
bool read_rom(Connection& c, const perf::SensorConfig& cfg, uint8_t rom[perf::kRomLen]) {
    uint16_t regs[perf::thermux::kRegsPerRom];
    uint8_t exc = 0;
    uint16_t start = perf::thermux::kRomStart +
                     perf::thermux::kRegsPerRom * static_cast<uint16_t>(perf::thermux::channel_of(cfg));
    if (c.read(cfg.unit_id, mbtcp::kFcReadInput, start, perf::thermux::kRegsPerRom, regs, &exc) !=
        Error::None) {
        return false;
    }
    perf::thermux::rom_from_regs(regs, rom);
    for (size_t i = 0; i < perf::kRomLen; ++i) {
        if (rom[i]) return true;
    }
    return false;
}

bool transport_error(Error e) {
    return e == Error::Resolve || e == Error::Connect || e == Error::Timeout ||
           e == Error::Protocol;
}

bool is_hex16(const char* s) {
    if (!s) return false;
    for (int i = 0; i < 16; ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return s[16] == '\0';
}

uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
}

void rom_from_hex(const char* hex, uint8_t rom[perf::kRomLen]) {
    for (size_t i = 0; i < perf::kRomLen; ++i) {
        rom[i] = static_cast<uint8_t>((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
    }
}

perf::BacnetObjectType perf_bacnet_type(bacnet::ObjectType t) {
    return t == bacnet::ObjectType::AnalogValue ? perf::BacnetObjectType::AnalogValue
                                                : perf::BacnetObjectType::AnalogInput;
}

bacnet::ObjectType bacnet_object_type(const perf::SensorConfig& cfg) {
    return cfg.bacnet_type == perf::BacnetObjectType::AnalogValue ? bacnet::ObjectType::AnalogValue
                                                                  : bacnet::ObjectType::AnalogInput;
}

float bacnet_to_celsius(float v, uint32_t units) {
    if (units == kBacnetUnitsF) return (v - 32.0f) * 5.0f / 9.0f;
    if (units == kBacnetUnitsK) return v - 273.15f;
    return v;
}

bool bacnet_status_fault(uint8_t bits) {
    // BACnet status-flags bit order is MSB first in the first octet:
    // in-alarm, fault, overridden, out-of-service.
    return (bits & 0x40) != 0 || (bits & 0x10) != 0;
}

class BacnetClient {
public:
    ~BacnetClient() { close(); }

    Error open(const char* host, uint16_t port) {
        close();
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char port_s[6];
        snprintf(port_s, sizeof(port_s), "%u", port);
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host, port_s, &hints, &res) != 0 || res == nullptr) {
            if (res) freeaddrinfo(res);
            return Error::Resolve;
        }
        fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) {
            freeaddrinfo(res);
            return Error::Connect;
        }
        memcpy(&addr_, res->ai_addr, res->ai_addrlen);
        addr_len_ = res->ai_addrlen;
        freeaddrinfo(res);
        if (connect(fd_, reinterpret_cast<struct sockaddr*>(&addr_), addr_len_) != 0) {
            close();
            return Error::Connect;
        }
        struct timeval tv = {kIoTimeoutMs / 1000, (kIoTimeoutMs % 1000) * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        snprintf(host_, sizeof(host_), "%s", host);
        port_ = port;
        return Error::None;
    }

    bool is_open_to(const char* host, uint16_t port) const {
        return fd_ >= 0 && port_ == port && strcmp(host_, host) == 0;
    }

    Error request(const uint8_t* req, size_t req_len, uint8_t* resp, size_t resp_cap, size_t* resp_len) {
        if (fd_ < 0) return Error::Connect;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (send(fd_, req, req_len, 0) != static_cast<int>(req_len)) {
                return Error::Timeout;
            }
            int n = recv(fd_, resp, resp_cap, 0);
            if (n > 0) {
                *resp_len = static_cast<size_t>(n);
                return Error::None;
            }
        }
        close();
        return Error::Timeout;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    struct sockaddr_storage addr_ = {};
    socklen_t addr_len_ = 0;
    char host_[sizeof(perf::SensorConfig::host)] = {};
    uint16_t port_ = 0;
};

struct BacnetRead {
    Error error = Error::None;
    uint8_t exception = 0;
    uint8_t error_class = 0;
    float celsius = 0.0f;
    char object_name[41] = {};
    char rom_hex[17] = {};
    uint32_t units = 0;
    uint32_t reliability = kBacnetReliabilityUnknown;
    bool reliability_known = false;
    bool device_known = false;
    uint32_t device_instance = perf::kBacnetDeviceWildcard;
};

void map_bacnet_parse(bacnet::Parse p, const bacnet::ErrorInfo& info, BacnetRead* out) {
    if (p == bacnet::Parse::Error) {
        out->error = Error::Rejected;
        out->error_class = info.error_class;
        out->exception = info.error_code;
    } else if (p == bacnet::Parse::Reject || p == bacnet::Parse::Abort) {
        out->error = Error::Rejected;
        out->exception = info.reason;
    } else if (p == bacnet::Parse::NotFound) {
        out->error = Error::NoReading;
    } else {
        out->error = Error::Protocol;
    }
}

Error bacnet_read_property_value(BacnetClient& c, bacnet::ObjectType type, uint32_t instance,
                                 uint32_t prop, bacnet::Value* value, uint8_t* exception,
                                 uint8_t* error_class) {
    uint8_t invoke = ++s_bacnet_invoke;
    if (invoke == 0) invoke = ++s_bacnet_invoke;
    uint8_t req[128];
    size_t req_len = 0;
    if (!bacnet::build_read_property(invoke, type, instance, prop, 0, false, req, sizeof(req),
                                     &req_len)) {
        return Error::Protocol;
    }
    uint8_t resp[bacnet::kMaxFrame];
    size_t resp_len = 0;
    Error e = c.request(req, req_len, resp, sizeof(resp), &resp_len);
    if (e != Error::None) return e;
    bacnet::Value v;
    bacnet::ErrorInfo info;
    bacnet::Parse p = bacnet::parse_read_property_ack(resp, resp_len, invoke, prop, value, &info);
    if (p == bacnet::Parse::Ok) return Error::None;
    if (p == bacnet::Parse::Error) {
        if (exception) *exception = info.error_code;
        if (error_class) *error_class = info.error_class;
        return Error::Rejected;
    }
    if (p == bacnet::Parse::Reject || p == bacnet::Parse::Abort) {
        if (exception) *exception = info.reason;
        return Error::Rejected;
    }
    return Error::Protocol;
}

void apply_bacnet_value(uint32_t prop, const bacnet::Value& v, BacnetRead* out,
                        bool* have_value, bool* have_units, bool* have_status,
                        uint8_t* status_bits) {
    switch (prop) {
        case bacnet::PROP_OBJECT_NAME:
            if (v.type == bacnet::ValueType::String) strlcpy(out->object_name, v.str, sizeof(out->object_name));
            break;
        case bacnet::PROP_PRESENT_VALUE:
            if (v.type == bacnet::ValueType::Real) {
                out->celsius = v.real;
                *have_value = true;
            }
            break;
        case bacnet::PROP_UNITS:
            if (v.type == bacnet::ValueType::Enumerated) {
                out->units = v.u;
                *have_units = true;
            }
            break;
        case bacnet::PROP_STATUS_FLAGS:
            if (v.type == bacnet::ValueType::BitString && v.bit_count >= 4) {
                *status_bits = v.bits;
                *have_status = true;
            }
            break;
        case bacnet::PROP_RELIABILITY:
            if (v.type == bacnet::ValueType::Enumerated) {
                out->reliability = v.u;
                out->reliability_known = true;
            }
            break;
        case bacnet::PROP_DESCRIPTION:
            if (v.type == bacnet::ValueType::String && is_hex16(v.str)) {
                strlcpy(out->rom_hex, v.str, sizeof(out->rom_hex));
            }
            break;
    }
}

BacnetRead bacnet_read_device_instance(BacnetClient& c) {
    BacnetRead out;
    bacnet::Value v;
    uint8_t exc = 0, cls = 0;
    out.error = bacnet_read_property_value(c, bacnet::ObjectType::Device, bacnet::kDeviceWildcard,
                                           bacnet::PROP_OBJECT_IDENTIFIER, &v, &exc, &cls);
    out.exception = exc;
    out.error_class = cls;
    if (out.error != Error::None) return out;
    if (v.type == bacnet::ValueType::ObjectId && v.object.type == bacnet::ObjectType::Device) {
        out.device_known = true;
        out.device_instance = v.object.instance;
    } else {
        out.error = Error::Protocol;
    }
    return out;
}

BacnetRead bacnet_read_value(BacnetClient& c, const perf::SensorConfig& cfg) {
    BacnetRead out;
    BacnetRead dev = bacnet_read_device_instance(c);
    if (dev.error != Error::None) return dev;
    out.device_known = dev.device_known;
    out.device_instance = dev.device_instance;
    if (cfg.bacnet_device_known && out.device_instance != cfg.bacnet_device_instance) {
        out.error = Error::WrongDevice;
        return out;
    }
    const uint32_t props[] = {bacnet::PROP_OBJECT_NAME, bacnet::PROP_PRESENT_VALUE,
                              bacnet::PROP_UNITS, bacnet::PROP_STATUS_FLAGS,
                              bacnet::PROP_RELIABILITY, bacnet::PROP_DESCRIPTION};
    uint8_t invoke = ++s_bacnet_invoke;
    if (invoke == 0) invoke = ++s_bacnet_invoke;
    uint8_t req[256];
    size_t req_len = 0;
    if (!bacnet::build_read_property_multiple(invoke, bacnet_object_type(cfg), cfg.bacnet_instance,
                                              props, sizeof(props) / sizeof(props[0]), req,
                                              sizeof(req), &req_len)) {
        out.error = Error::Protocol;
        return out;
    }
    uint8_t resp[bacnet::kMaxFrame];
    size_t resp_len = 0;
    out.error = c.request(req, req_len, resp, sizeof(resp), &resp_len);
    if (out.error != Error::None) return out;

    bacnet::PropertyValue values[8];
    size_t count = 0;
    bacnet::ErrorInfo info;
    bacnet::Parse p = bacnet::parse_read_property_multiple_ack(resp, resp_len, invoke, values,
                                                               sizeof(values) / sizeof(values[0]),
                                                               &count, &info);
    if (p != bacnet::Parse::Ok) {
        if (p != bacnet::Parse::Reject && p != bacnet::Parse::Error) {
            map_bacnet_parse(p, info, &out);
            return out;
        }
        count = 0;
        for (uint32_t prop : props) {
            bacnet::Value v;
            uint8_t exc = 0, cls = 0;
            Error e = bacnet_read_property_value(c, bacnet_object_type(cfg), cfg.bacnet_instance,
                                                 prop, &v, &exc, &cls);
            if (e == Error::None && count < sizeof(values) / sizeof(values[0])) {
                values[count].object = {bacnet_object_type(cfg), cfg.bacnet_instance};
                values[count].property = prop;
                values[count].value = v;
                ++count;
            } else if (prop == bacnet::PROP_PRESENT_VALUE || prop == bacnet::PROP_UNITS ||
                       prop == bacnet::PROP_STATUS_FLAGS) {
                out.error = e;
                out.exception = exc;
                out.error_class = cls;
                return out;
            }
        }
    }
    bool have_value = false, have_units = false, have_status = false;
    uint8_t status_bits = 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[i].error) {
            if (values[i].property == bacnet::PROP_PRESENT_VALUE) out.error = Error::NoReading;
            continue;
        }
        apply_bacnet_value(values[i].property, values[i].value, &out, &have_value, &have_units,
                           &have_status, &status_bits);
    }
    if (out.error != Error::None) return out;
    if (!have_value) {
        out.error = Error::NoReading;
    } else if (!have_units || (out.units != kBacnetUnitsC && out.units != kBacnetUnitsF &&
                              out.units != kBacnetUnitsK)) {
        out.error = Error::Units;
    } else if (!have_status) {
        out.error = Error::Protocol;
    } else if ((out.reliability_known && out.reliability != kBacnetReliabilityNoFault) ||
               bacnet_status_fault(status_bits)) {
        out.error = Error::NoReading;
    } else {
        out.celsius = bacnet_to_celsius(out.celsius, out.units);
        if (!(out.celsius >= perf::kMinPlausibleC && out.celsius <= perf::kMaxPlausibleC)) {
            out.error = Error::OutOfRange;
        }
    }
    return out;
}

// Worker-owned connections kept open between polls: reconnecting every poll
// leaves a TIME_WAIT socket behind each time, which holds a steady few KB of
// scarce internal RAM, and needlessly churns the sensor's connection slots.
Connection s_conns[perf::kSlotCount];

void poll_bacnet_slot(int i, const perf::SensorConfig& cfg, bool net_up) {
    BacnetRead r;
    if (!net_up) {
        r.error = Error::Connect;
    } else {
        BacnetClient c;
        r.error = c.open(cfg.host, cfg.port);
        if (r.error == Error::None) r = bacnet_read_value(c, cfg);
    }

    uint8_t rom[perf::kRomLen] = {};
    if (r.error == Error::None && is_hex16(r.rom_hex)) {
        rom_from_hex(r.rom_hex, rom);
        lock();
        const perf::SensorConfig& cur = s_settings.sensors[i];
        if (perf::same_source(cur, cfg)) {
            if (cur.rom_known && memcmp(cur.rom, rom, perf::kRomLen) != 0) {
                r.error = Error::SensorChanged;
            }
        }
        unlock();
    }

    uint32_t now = now_ms();
    lock();
    if (perf::same_source(s_settings.sensors[i], cfg)) {
        Slot& slot = s_slots[i];
        slot.status.error = r.error;
        slot.status.exception = r.exception;
        slot.status.error_class = r.error_class;
        strlcpy(slot.status.object_name, r.object_name, sizeof(slot.status.object_name));
        strlcpy(slot.status.rom_hex, r.rom_hex, sizeof(slot.status.rom_hex));
        slot.status.bacnet_device_known = r.device_known;
        slot.status.bacnet_device_instance = r.device_instance;
        slot.thermux = ThermuxState::No;
        if (r.error == Error::None) {
            slot.status.has_reading = true;
            slot.status.celsius = r.celsius;
            slot.last_ok_ms = now;
            s_selector.on_reading(static_cast<perf::Slot>(i), now, r.celsius);
        } else {
            s_selector.on_error(static_cast<perf::Slot>(i), now);
        }
    }
    unlock();
    if (r.error != Error::None) {
        ESP_LOGD(TAG, "BACnet sensor %d (%s:%u object %lu): %s", i, cfg.host, cfg.port,
                 (unsigned long)cfg.bacnet_instance, error_name(r.error));
    }
}

void poll_slot(int i, const perf::SensorConfig& cfg, Connection& c, bool net_up) {
    ValueRead r = {Error::Connect, 0, 0.0f};
    if (!net_up) {
        c.close();
    } else {
        bool reused = c.is_open_to(cfg.host, cfg.port);
        r.error = reused ? Error::None : c.open(cfg.host, cfg.port);
        if (r.error == Error::None) r = read_value(c, cfg);
        // The sensor may have dropped an idle connection; retry once fresh
        // before reporting a failure.
        if (reused && transport_error(r.error)) {
            r = {c.open(cfg.host, cfg.port), 0, 0.0f};
            if (r.error == Error::None) r = read_value(c, cfg);
        }
    }

    lock();
    ThermuxState thermux = s_slots[i].thermux;
    unlock();

    bool learned = false;
    uint8_t rom[perf::kRomLen];
    if (r.error == Error::None && perf::thermux::candidate(cfg)) {
        if (thermux == ThermuxState::Unknown) {
            thermux = probe_thermux(c, cfg) ? ThermuxState::Yes : ThermuxState::No;
        }
        if (thermux == ThermuxState::Yes && read_rom(c, cfg, rom)) {
            lock();
            const perf::SensorConfig& cur = s_settings.sensors[i];
            if (perf::same_source(cur, cfg)) {
                if (!cur.rom_known) {
                    learned = true;
                } else if (memcmp(cur.rom, rom, perf::kRomLen) != 0) {
                    r.error = Error::SensorChanged;
                }
            }
            unlock();
        }
    }

    uint32_t now = now_ms();
    lock();
    // The settings may have changed while this read was in flight.
    if (perf::same_source(s_settings.sensors[i], cfg)) {
        Slot& slot = s_slots[i];
        slot.status.error = r.error;
        slot.status.exception = r.exception;
        slot.thermux = (r.error == Error::Connect || r.error == Error::Resolve) ? ThermuxState::Unknown
                                                                              : thermux;
        if (r.error == Error::None) {
            slot.status.has_reading = true;
            slot.status.celsius = r.celsius;
            slot.last_ok_ms = now;
            s_selector.on_reading(static_cast<perf::Slot>(i), now, r.celsius);
        } else {
            s_selector.on_error(static_cast<perf::Slot>(i), now);
        }
        if (learned) {
            s_settings.sensors[i].rom_known = true;
            memcpy(s_settings.sensors[i].rom, rom, perf::kRomLen);
            s_save_pending = true;
        }
    }
    unlock();
    if (r.error != Error::None) {
        ESP_LOGD(TAG, "Sensor %d (%s:%u reg %u): %s", i, cfg.host, cfg.port, cfg.address,
                 error_name(r.error));
    }
}

bool value_reachable(Error e) {
    return e == Error::None || e == Error::NoReading || e == Error::OutOfRange;
}

// Illegal function (1) or illegal data address (2): the device may keep this
// value in the other register table.
bool wrong_register_table(const ValueRead& r) {
    return r.error == Error::Exception && (r.exception == 1 || r.exception == 2);
}

void run_test(const perf::SensorConfig& requested, TestResult* out) {
    *out = {};
    out->error = Error::None;
    out->thermux_age_s = 0xFFFF;
    out->reg_type = requested.reg_type;
    if (requested.source == perf::SensorSource::BacnetIp) {
        BacnetClient c;
        Error e = network_up() ? c.open(requested.host, requested.port) : Error::Connect;
        if (e != Error::None) {
            out->error = e;
            return;
        }
        BacnetRead r = bacnet_read_value(c, requested);
        out->error = r.error;
        out->exception = r.exception;
        out->error_class = r.error_class;
        out->celsius = r.celsius;
        out->bacnet_units = r.units;
        out->bacnet_reliability = r.reliability;
        out->bacnet_reliability_known = r.reliability_known;
        out->bacnet_device_known = r.device_known;
        out->bacnet_device_instance = r.device_instance;
        strlcpy(out->object_name, r.object_name, sizeof(out->object_name));
        if (is_hex16(r.rom_hex)) {
            out->rom_valid = true;
            strlcpy(out->rom_hex, r.rom_hex, sizeof(out->rom_hex));
        }
        return;
    }
    Connection c;
    Error e = network_up() ? c.open(requested.host, requested.port) : Error::Connect;
    if (e != Error::None) {
        out->error = e;
        return;
    }
    perf::SensorConfig cfg = requested;
    ValueRead r = read_value(c, cfg);
    if (wrong_register_table(r)) {
        perf::SensorConfig other = cfg;
        other.reg_type = cfg.reg_type == perf::RegisterType::Input ? perf::RegisterType::Holding
                                                                   : perf::RegisterType::Input;
        ValueRead r2 = read_value(c, other);
        if (r2.error == Error::None) {
            cfg = other;
            r = r2;
            out->reg_type = other.reg_type;
        }
    }
    out->error = r.error;
    out->exception = r.exception;
    out->celsius = r.celsius;
    if (!value_reachable(r.error) || !perf::thermux::candidate(cfg) || !probe_thermux(c, cfg)) return;

    out->thermux = true;
    out->channel = perf::thermux::channel_of(cfg);
    uint16_t v = 0;
    uint8_t exc = 0;
    if (c.read(cfg.unit_id, mbtcp::kFcReadInput, perf::thermux::kStatusStart + out->channel, 1, &v,
               &exc) == Error::None) {
        out->thermux_status = static_cast<uint8_t>(v);
    }
    constexpr uint16_t kAgeStart = 300;
    if (c.read(cfg.unit_id, mbtcp::kFcReadInput, kAgeStart + out->channel, 1, &v, &exc) ==
        Error::None) {
        out->thermux_age_s = v;
    }
    uint8_t rom[perf::kRomLen];
    if (read_rom(c, cfg, rom)) {
        out->rom_valid = true;
        for (size_t i = 0; i < perf::kRomLen; ++i) {
            snprintf(out->rom_hex + 2 * i, 3, "%02X", rom[i]);
        }
    }
}

void worker(void*) {
    uint32_t next_poll = now_ms();
    bool net_seen = false;
    for (;;) {
        int32_t wait = static_cast<int32_t>(next_poll - now_ms());
        if (wait > 0) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));

        lock();
        bool test = s_test_pending;
        uint32_t ticket = s_test_ticket;
        perf::SensorConfig test_cfg = s_test_cfg;
        unlock();
        if (test) {
            TestResult result;
            run_test(test_cfg, &result);
            lock();
            s_test_result = result;
            s_test_done_ticket = ticket;
            s_test_pending = false;
            unlock();
            log_stack_watermark("test");
        }

        lock();
        bool browse = s_browse_pending;
        uint32_t browse_ticket = s_browse_ticket;
        char browse_host[perf::kHostMax];
        strlcpy(browse_host, s_browse_host, sizeof(browse_host));
        uint16_t browse_port = s_browse_port;
        BrowseResult* browse_out = s_browse_out;
        unlock();
        if (browse && browse_out) {
            run_browse_internal(browse_host, browse_port, browse_out, kBrowseMaxMs);
            lock();
            s_browse_done_ticket = browse_ticket;
            s_browse_pending = false;
            s_browse_busy = false;
            unlock();
            log_stack_watermark("browse");
        }

        if (static_cast<int32_t>(now_ms() - next_poll) < 0) continue;
        // Until the first connection the sensors stay "not read" rather than
        // failing, so a normal boot doesn't briefly fall back.
        bool net_up = network_up();
        net_seen = net_seen || net_up;
        if (!net_seen) {
            next_poll = now_ms() + 1000;
            continue;
        }
        lock();
        perf::Settings cfg = s_settings;
        unlock();
        for (int i = 0; i < perf::kSlotCount; ++i) {
            const perf::SensorConfig& sc = cfg.sensors[i];
            if (sc.source == perf::SensorSource::BacnetIp) {
                s_conns[i].close();
                poll_bacnet_slot(i, sc, net_up);
                continue;
            }
            if (sc.source != perf::SensorSource::ModbusTcp) {
                s_conns[i].close();
                continue;
            }
            // Both sensors are usually channels on one Thermux: share a link.
            int ci = i;
            for (int j = 0; j < i; ++j) {
                const perf::SensorConfig& o = cfg.sensors[j];
                if (o.source == perf::SensorSource::ModbusTcp && o.port == sc.port &&
                    strcmp(o.host, sc.host) == 0) {
                    ci = j;
                    break;
                }
            }
            if (ci != i) s_conns[i].close();
            poll_slot(i, sc, s_conns[ci], net_up);
        }
        log_stack_watermark("poll");
        next_poll = now_ms() + kPollMs;
    }
}

void ensure_worker() {
    if (s_task || !s_worker_allowed) return;
    // PSRAM stack, like the weather and HA workers: a resident internal-RAM
    // stack created this early fragments the internal heap enough that the
    // HTTPS server can't get its task stack (ESP_ERR_HTTPD_TASK).
    if (xTaskCreateWithCaps(worker, "ext_temp", kWorkerStack, nullptr, 3, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the external sensor task");
        s_task = nullptr;
    }
}

void reset_slots_locked() {
    s_selector.reset();
    for (int i = 0; i < perf::kSlotCount; ++i) {
        s_slots[i] = {};
        s_slots[i].status.configured =
            s_settings.sensors[i].source == perf::SensorSource::ModbusTcp ||
            s_settings.sensors[i].source == perf::SensorSource::BacnetIp;
        s_slots[i].status.error = Error::NotRead;
    }
}

}  // namespace

const char* error_name(Error e) {
    switch (e) {
        case Error::None: return "none";
        case Error::NotRead: return "not_read";
        case Error::Resolve: return "resolve";
        case Error::Connect: return "connect";
        case Error::Timeout: return "timeout";
        case Error::Protocol: return "protocol";
        case Error::Exception: return "exception";
        case Error::Rejected: return "rejected";
        case Error::Units: return "units";
        case Error::NoReading: return "no_reading";
        case Error::OutOfRange: return "out_of_range";
        case Error::SensorChanged: return "sensor_changed";
        case Error::WrongDevice: return "wrong_device";
        case Error::Busy: return "busy";
    }
    return "unknown";
}

void init(bool allow_worker) {
    if (s_mutex) return;
    s_worker_allowed = allow_worker;
    s_mutex = xSemaphoreCreateMutex();
    s_save_mutex = xSemaphoreCreateMutex();
    s_settings = perf::load();
    lock();
    reset_slots_locked();
    unlock();
    if (perf::uses_network(s_settings)) ensure_worker();
    ESP_LOGI(TAG, "Flow %.1f L/min, %s %u%%, supply %s, return %s", s_settings.flow_lpm_x10 / 10.0f,
             perf::fluid_name(s_settings.fluid), s_settings.glycol_pct,
             perf::source_name(s_settings.sensors[0].source),
             perf::source_name(s_settings.sensors[1].source));
}

perf::Settings settings() {
    if (!s_mutex) return perf::defaults();
    lock();
    perf::Settings s = s_settings;
    unlock();
    return s;
}

void service() {
    if (!s_mutex) return;
    xSemaphoreTake(s_save_mutex, portMAX_DELAY);
    lock();
    bool pending = s_save_pending;
    s_save_pending = false;
    perf::Settings s = s_settings;
    unlock();
    if (pending && !perf::save(s)) {
        lock();
        s_save_pending = true;  // try again on a later call
        unlock();
    }
    xSemaphoreGive(s_save_mutex);
}

perf::Invalid apply_settings(const perf::Settings& in, const bool sensor_edited[perf::kSlotCount],
                             bool* saved) {
    if (saved) *saved = false;
    perf::Invalid v = perf::validate(in);
    if (v != perf::Invalid::None || !s_mutex) return v;
    xSemaphoreTake(s_save_mutex, portMAX_DELAY);
    lock();
    perf::Settings next = in;
    bool sensors_changed = false;
    for (int i = 0; i < perf::kSlotCount; ++i) {
        perf::SensorConfig& s = next.sensors[i];
        const perf::SensorConfig& cur = s_settings.sensors[i];
        if (sensor_edited[i] || !perf::same_source(s, cur)) {
            if (s.source != perf::SensorSource::BacnetIp) {
                s.rom_known = false;
                memset(s.rom, 0, sizeof(s.rom));
            }
        } else {
            s.rom_known = cur.rom_known;
            memcpy(s.rom, cur.rom, sizeof(s.rom));
            s.bacnet_device_known = cur.bacnet_device_known;
            s.bacnet_device_instance = cur.bacnet_device_instance;
            strlcpy(s.bacnet_object_name, cur.bacnet_object_name, sizeof(s.bacnet_object_name));
        }
        if (sensor_edited[i] || !perf::same_source(s, cur) || s.scale_exp != cur.scale_exp ||
            s.no_reading != cur.no_reading) {
            sensors_changed = true;
        }
    }
    unlock();
    if (!perf::save(next)) {
        xSemaphoreGive(s_save_mutex);
        return perf::Invalid::None;
    }
    if (saved) *saved = true;
    lock();
    s_settings = next;
    if (sensors_changed) reset_slots_locked();
    unlock();
    xSemaphoreGive(s_save_mutex);
    if (perf::uses_network(next)) {
        ensure_worker();
        if (s_task && sensors_changed) xTaskNotifyGive(s_task);
    }
    return perf::Invalid::None;
}

void slot_status(SlotStatus out[perf::kSlotCount]) {
    if (!s_mutex) {
        for (int i = 0; i < perf::kSlotCount; ++i) out[i] = {};
        return;
    }
    uint32_t now = now_ms();
    lock();
    for (int i = 0; i < perf::kSlotCount; ++i) {
        out[i] = s_slots[i].status;
        out[i].age_s = out[i].has_reading ? (now - s_slots[i].last_ok_ms) / 1000 : 0;
    }
    unlock();
}

perf::Selection choose_source(const perf::HeatPumpContext& hp) {
    if (!s_mutex) return perf::Selection{};
    lock();
    bool configured[perf::kSlotCount];
    for (int i = 0; i < perf::kSlotCount; ++i) {
        configured[i] = s_settings.sensors[i].source == perf::SensorSource::ModbusTcp ||
                        s_settings.sensors[i].source == perf::SensorSource::BacnetIp;
    }
    perf::Selection sel = s_selector.evaluate(now_ms(), configured, hp);
    unlock();
    return sel;
}

uint32_t test_start(const perf::SensorConfig& cfg) {
    if (!s_mutex) return 0;
    ensure_worker();
    if (!s_task) return 0;
    lock();
    if (s_test_pending) {
        unlock();
        return 0;
    }
    s_test_cfg = cfg;
    s_test_pending = true;
    uint32_t ticket = ++s_test_ticket;
    if (ticket == 0) ticket = ++s_test_ticket;
    unlock();
    xTaskNotifyGive(s_task);
    return ticket;
}

bool test_result(uint32_t ticket, TestResult* out) {
    if (!s_mutex || ticket == 0) return false;
    lock();
    bool done = s_test_done_ticket == ticket;
    if (done) *out = s_test_result;
    unlock();
    return done;
}

bool test_blocking(const perf::SensorConfig& cfg, TestResult* out, uint32_t timeout_ms) {
    uint32_t ticket = test_start(cfg);
    if (ticket == 0) {
        *out = {};
        out->error = Error::Busy;
        return true;
    }
    uint32_t start = now_ms();
    uint32_t wait_ms = timeout_ms + 1500;
    while (now_ms() - start < wait_ms) {
        if (test_result(ticket, out)) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static Error bacnet_read_property(BacnetClient& c, bacnet::ObjectType type, uint32_t instance,
                                  uint32_t prop, uint32_t array_index, bool has_array_index,
                                  bacnet::Value* value, uint8_t* exception, uint8_t* error_class)
{
    uint8_t invoke = ++s_bacnet_invoke;
    if (invoke == 0) invoke = ++s_bacnet_invoke;
    uint8_t req[128];
    size_t req_len = 0;
    if (!bacnet::build_read_property(invoke, type, instance, prop, array_index, has_array_index,
                                     req, sizeof(req), &req_len)) {
        return Error::Protocol;
    }
    uint8_t resp[bacnet::kMaxFrame];
    size_t resp_len = 0;
    Error e = c.request(req, req_len, resp, sizeof(resp), &resp_len);
    if (e != Error::None) return e;
    bacnet::ErrorInfo info;
    bacnet::Parse p = bacnet::parse_read_property_ack(resp, resp_len, invoke, prop, value, &info);
    if (p == bacnet::Parse::Ok) return Error::None;
    if (p == bacnet::Parse::Error) {
        if (exception) *exception = info.error_code;
        if (error_class) *error_class = info.error_class;
        return Error::Rejected;
    }
    if (p == bacnet::Parse::Reject || p == bacnet::Parse::Abort) {
        if (exception) *exception = info.reason;
        return Error::Rejected;
    }
    return Error::Protocol;
}

bool run_browse_internal(const char* host, uint16_t port, BrowseResult* out, uint32_t timeout_ms)
{
    *out = {};
    auto finish = [&]() { return true; };
    if (!host || !perf::host_valid(host) || port == 0) {
        out->error = Error::Resolve;
        return finish();
    }
    uint32_t budget_ms = timeout_ms > 0 && timeout_ms < kBrowseMaxMs ? timeout_ms : kBrowseMaxMs;
    const uint32_t deadline = now_ms() + budget_ms;
    BacnetClient c;
    out->error = network_up() ? c.open(host, port) : Error::Connect;
    if (out->error != Error::None) return finish();

    bacnet::Value v;
    // Direct unicast does not need a configured device instance: ask the
    // wildcard device for its object identifier first, then page Object_List.
    out->error = bacnet_read_property(c, bacnet::ObjectType::Device, bacnet::kDeviceWildcard,
                                      bacnet::PROP_OBJECT_IDENTIFIER, 0, false, &v,
                                      &out->exception, &out->error_class);
    uint32_t device_instance = bacnet::kDeviceWildcard;
    if (out->error == Error::None && v.type == bacnet::ValueType::ObjectId) {
        device_instance = v.object.instance;
        out->device_instance_known = true;
        out->device_instance = device_instance;
    } else if (out->error != Error::None) {
        return finish();
    }

    if (bacnet_read_property(c, bacnet::ObjectType::Device, device_instance, bacnet::PROP_OBJECT_NAME,
                             0, false, &v, &out->exception, &out->error_class) == Error::None &&
        v.type == bacnet::ValueType::String) {
        strlcpy(out->device_name, v.str, sizeof(out->device_name));
    }
    if (bacnet_read_property(c, bacnet::ObjectType::Device, device_instance, bacnet::PROP_MODEL_NAME,
                             0, false, &v, &out->exception, &out->error_class) == Error::None &&
        v.type == bacnet::ValueType::String) {
        strlcpy(out->model_name, v.str, sizeof(out->model_name));
    }

    out->error = bacnet_read_property(c, bacnet::ObjectType::Device, device_instance,
                                      bacnet::PROP_OBJECT_LIST, 0, true, &v, &out->exception,
                                      &out->error_class);
    if (out->error != Error::None || v.type != bacnet::ValueType::Unsigned) {
        if (out->error == Error::None) out->error = Error::Protocol;
        return finish();
    }
    out->total_objects = v.u;
    uint32_t scan_limit = out->total_objects;
    if (scan_limit > kBrowseMaxObjects) scan_limit = kBrowseMaxObjects;
    const uint32_t props[] = {bacnet::PROP_OBJECT_NAME, bacnet::PROP_PRESENT_VALUE,
                              bacnet::PROP_UNITS, bacnet::PROP_RELIABILITY,
                              bacnet::PROP_STATUS_FLAGS, bacnet::PROP_DESCRIPTION};
    for (uint32_t idx = 1; idx <= scan_limit && out->count < kBrowseMaxResults; ++idx) {
        if (static_cast<int32_t>(now_ms() - deadline) >= 0) {
            out->truncated = true;
            break;
        }
        out->scanned = idx;
        if (bacnet_read_property(c, bacnet::ObjectType::Device, device_instance,
                                 bacnet::PROP_OBJECT_LIST, idx, true, &v, &out->exception,
                                 &out->error_class) != Error::None ||
            v.type != bacnet::ValueType::ObjectId) {
            continue;
        }
        if (v.object.type != bacnet::ObjectType::AnalogInput &&
            v.object.type != bacnet::ObjectType::AnalogValue) {
            continue;
        }
        uint8_t invoke = ++s_bacnet_invoke;
        if (invoke == 0) invoke = ++s_bacnet_invoke;
        uint8_t req[192];
        size_t req_len = 0;
        if (!bacnet::build_read_property_multiple(invoke, v.object.type, v.object.instance, props,
                                                  sizeof(props) / sizeof(props[0]), req,
                                                  sizeof(req), &req_len)) {
            continue;
        }
        uint8_t resp[bacnet::kMaxFrame];
        size_t resp_len = 0;
        if (c.request(req, req_len, resp, sizeof(resp), &resp_len) != Error::None) continue;
        bacnet::PropertyValue vals[8];
        size_t nvals = 0;
        bacnet::ErrorInfo info;
        if (bacnet::parse_read_property_multiple_ack(resp, resp_len, invoke, vals,
                                                     sizeof(vals) / sizeof(vals[0]), &nvals,
                                                     &info) != bacnet::Parse::Ok) {
            continue;
        }
        BrowseSensor& bs = out->sensors[out->count];
        bs.object_type = perf_bacnet_type(v.object.type);
        bs.object_instance = v.object.instance;
        bool have_value = false, have_units = false, have_status = false;
        uint8_t status_bits = 0;
        bs.reliability = kBacnetReliabilityUnknown;
        for (size_t j = 0; j < nvals; ++j) {
            if (vals[j].error) continue;
            const bacnet::Value& pv = vals[j].value;
            switch (vals[j].property) {
                case bacnet::PROP_OBJECT_NAME:
                    if (pv.type == bacnet::ValueType::String) strlcpy(bs.object_name, pv.str, sizeof(bs.object_name));
                    break;
                case bacnet::PROP_PRESENT_VALUE:
                    if (pv.type == bacnet::ValueType::Real) {
                        bs.celsius = pv.real;
                        have_value = true;
                    }
                    break;
                case bacnet::PROP_UNITS:
                    if (pv.type == bacnet::ValueType::Enumerated) {
                        bs.units = pv.u;
                        have_units = true;
                    }
                    break;
                case bacnet::PROP_RELIABILITY:
                    if (pv.type == bacnet::ValueType::Enumerated) {
                        bs.reliability = pv.u;
                        bs.reliability_known = true;
                    }
                    break;
                case bacnet::PROP_STATUS_FLAGS:
                    if (pv.type == bacnet::ValueType::BitString && pv.bit_count >= 4) {
                        status_bits = pv.bits;
                        have_status = true;
                    }
                    break;
                case bacnet::PROP_DESCRIPTION:
                    if (pv.type == bacnet::ValueType::String && is_hex16(pv.str)) {
                        bs.rom_valid = true;
                        strlcpy(bs.rom_hex, pv.str, sizeof(bs.rom_hex));
                    }
                    break;
            }
        }
        if (!have_units ||
            (bs.units != kBacnetUnitsC && bs.units != kBacnetUnitsF && bs.units != kBacnetUnitsK)) {
            continue;
        }
        if (have_value) bs.celsius = bacnet_to_celsius(bs.celsius, bs.units);
        bs.available = have_value && have_status && !bacnet_status_fault(status_bits) &&
                       (!bs.reliability_known || bs.reliability == kBacnetReliabilityNoFault) &&
                       bs.celsius >= perf::kMinPlausibleC && bs.celsius <= perf::kMaxPlausibleC;
        ++out->count;
    }
    if (out->total_objects > out->scanned || out->scanned >= kBrowseMaxObjects ||
        out->count >= kBrowseMaxResults) {
        out->truncated = true;
    }
    out->error = Error::None;
    return finish();
}

bool browse_blocking(const char* host, uint16_t port, BrowseResult* out, uint32_t timeout_ms)
{
    *out = {};
    if (!s_mutex) {
        out->error = Error::Busy;
        return true;
    }
    ensure_worker();
    if (!s_task) {
        out->error = Error::Busy;
        return true;
    }
    lock();
    if (s_browse_busy) {
        unlock();
        out->error = Error::Busy;
        return true;
    }
    s_browse_busy = true;
    s_browse_pending = true;
    s_browse_out = out;
    strlcpy(s_browse_host, host ? host : "", sizeof(s_browse_host));
    s_browse_port = port;
    uint32_t ticket = ++s_browse_ticket;
    if (ticket == 0) ticket = ++s_browse_ticket;
    unlock();
    xTaskNotifyGive(s_task);
    uint32_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        lock();
        bool done = s_browse_done_ticket == ticket;
        unlock();
        if (done) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

}  // namespace ext_temp
