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

#include "mbtcp_frame.h"
#include "wifi_manager.h"

namespace ext_temp {

namespace {

const char* TAG = "ext_temp";

constexpr uint32_t kPollMs = 10000;
constexpr uint32_t kIoTimeoutMs = 1500;
constexpr uint32_t kWorkerStack = 4608;

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

// Test request/response, guarded by s_mutex.
bool s_test_pending = false;
uint32_t s_test_ticket = 0;
uint32_t s_test_done_ticket = 0;
perf::SensorConfig s_test_cfg;
TestResult s_test_result;

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

// Worker-owned connections kept open between polls: reconnecting every poll
// leaves a TIME_WAIT socket behind each time, which holds a steady few KB of
// scarce internal RAM, and needlessly churns the sensor's connection slots.
Connection s_conns[perf::kSlotCount];

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

void run_test(const perf::SensorConfig& cfg, TestResult* out) {
    *out = {};
    out->error = Error::None;
    out->thermux_age_s = 0xFFFF;
    Connection c;
    Error e = network_up() ? c.open(cfg.host, cfg.port) : Error::Connect;
    if (e != Error::None) {
        out->error = e;
        return;
    }
    ValueRead r = read_value(c, cfg);
    out->error = r.error;
    out->exception = r.exception;
    out->celsius = r.celsius;
    bool reachable = r.error == Error::None || r.error == Error::NoReading ||
                     r.error == Error::OutOfRange;
    if (!reachable || !perf::thermux::candidate(cfg) || !probe_thermux(c, cfg)) return;

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
            s_settings.sensors[i].source == perf::SensorSource::ModbusTcp;
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
        case Error::NoReading: return "no_reading";
        case Error::OutOfRange: return "out_of_range";
        case Error::SensorChanged: return "sensor_changed";
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
            s.rom_known = false;
            memset(s.rom, 0, sizeof(s.rom));
        } else {
            s.rom_known = cur.rom_known;
            memcpy(s.rom, cur.rom, sizeof(s.rom));
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
        configured[i] = s_settings.sensors[i].source == perf::SensorSource::ModbusTcp;
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
    while (now_ms() - start < timeout_ms) {
        if (test_result(ticket, out)) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

}  // namespace ext_temp
