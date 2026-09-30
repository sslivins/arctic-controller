// Native unit tests for the heat output & COP sensor path:
//   perf_settings.cpp  -- config validation, value decoding, NVS blob
//   mbtcp_frame.cpp    -- Modbus TCP read request/response framing
//   perf_source.cpp    -- median filter, external/heat-pump source selection
//
// Nothing here is safety-critical, but a wrong answer is silent: a mis-decoded
// register or a filter that never falls back just shows a plausible-looking
// COP that is wrong. So the decoding is checked against hand-built frames, and
// the fallback/recovery timing against an explicit clock.

#include "nvs_fake.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mbtcp_frame.h"
#include "perf_settings.h"
#include "perf_source.h"

static int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                                   \
    do {                                                                                    \
        double a_ = (double)(actual);                                                       \
        double e_ = (double)(expected);                                                     \
        if (!(std::fabs(a_ - e_) <= (tol))) {                                               \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %g, expected %g\n", __FILE__, __LINE__, \
                         #actual, a_, e_);                                                  \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (0)

using namespace perf;

static SensorConfig modbus_sensor(const char* host, uint16_t reg) {
    SensorConfig s = default_sensor();
    s.source = SensorSource::ModbusTcp;
    std::strncpy(s.host, host, kHostMax - 1);
    s.address = reg;
    return s;
}

// ---- settings ----------------------------------------------------------------

static void test_defaults_are_valid() {
    Settings s = defaults();
    CHECK(validate(s) == Invalid::None);
    CHECK(s.flow_lpm_x10 == 400);
    CHECK(s.fluid == arctic::LoopFluid::Water);
    CHECK(s.sensors[0].source == SensorSource::HeatPump);
    CHECK(s.sensors[1].source == SensorSource::HeatPump);
    CHECK(!uses_network(s));
}

static void test_validation() {
    Settings s = defaults();
    s.flow_lpm_x10 = 5;
    CHECK(validate(s) == Invalid::Flow);
    s.flow_lpm_x10 = 3001;
    CHECK(validate(s) == Invalid::Flow);
    s = defaults();
    s.glycol_pct = 33;
    CHECK(validate(s) == Invalid::Glycol);
    s.glycol_pct = 65;
    CHECK(validate(s) == Invalid::Glycol);
    s.glycol_pct = 60;
    CHECK(validate(s) == Invalid::None);

    s = defaults();
    s.sensors[0].source = SensorSource::ModbusTcp;  // no host yet
    CHECK(validate(s) == Invalid::Host);
    std::strcpy(s.sensors[0].host, "thermux.local");
    CHECK(validate(s) == Invalid::None);
    CHECK(uses_network(s));
    s.sensors[0].port = 0;
    CHECK(validate(s) == Invalid::Port);
    s.sensors[0].port = 502;
    s.sensors[0].scale_exp = -4;
    CHECK(validate(s) == Invalid::Scale);
    s.sensors[0].scale_exp = 1;
    CHECK(validate(s) == Invalid::Scale);
    s.sensors[0].scale_exp = 0;
    s.sensors[0].value_type = ValueType::Float32;
    s.sensors[0].address = 0xFFFF;  // second register would be past the end
    CHECK(validate(s) == Invalid::Register);
    s.sensors[0].value_type = ValueType::Int16;
    CHECK(validate(s) == Invalid::None);

    // A heat-pump sensor keeps its Modbus fields, but they must be well-formed.
    s = defaults();
    std::strcpy(s.sensors[1].host, "bad host");
    CHECK(validate(s) == Invalid::Host);
}

static void test_host_valid() {
    CHECK(host_valid("192.168.10.60"));
    CHECK(host_valid("thermux.local"));
    CHECK(host_valid("my_thermux-2"));
    CHECK(!host_valid(""));
    CHECK(!host_valid("a b"));
    CHECK(!host_valid("-x"));
    CHECK(!host_valid("x."));
    CHECK(!host_valid("a..b"));
    CHECK(!host_valid("http://x"));
    std::string long_host(64, 'a');
    CHECK(!host_valid(long_host.c_str()));
    std::string max_host(63, 'a');
    CHECK(host_valid(max_host.c_str()));
}

static void test_names_roundtrip() {
    SensorSource src;
    CHECK(parse_source("modbus_tcp", &src) && src == SensorSource::ModbusTcp);
    CHECK(!parse_source("modbus", &src));
    arctic::LoopFluid f;
    CHECK(parse_fluid("propylene_glycol", &f) && f == arctic::LoopFluid::PropyleneGlycol);
    CHECK(std::strcmp(fluid_name(arctic::LoopFluid::EthyleneGlycol), "ethylene_glycol") == 0);
    ValueType vt;
    CHECK(parse_value_type("float32_swapped", &vt) && vt == ValueType::Float32Swapped);
    NoReading nr;
    CHECK(parse_no_reading("0x7fff", &nr) && nr == NoReading::X7FFF);
    RegisterType rt;
    CHECK(parse_register_type("holding", &rt) && rt == RegisterType::Holding);
    CHECK(!parse_register_type(nullptr, &rt));
}

static void test_decode() {
    SensorConfig s = modbus_sensor("t", 100);  // int16, x0.01, 0x8000 = none
    float c = 0;
    uint16_t r = static_cast<uint16_t>(int16_t(4523));
    CHECK(decode_value(s, &r, &c) == DecodeResult::Ok);
    CHECK_NEAR(c, 45.23, 1e-4);
    r = static_cast<uint16_t>(int16_t(-1250));
    CHECK(decode_value(s, &r, &c) == DecodeResult::Ok);
    CHECK_NEAR(c, -12.5, 1e-4);
    r = 0x8000;
    CHECK(decode_value(s, &r, &c) == DecodeResult::NoReading);
    r = 15000;  // 150 C
    CHECK(decode_value(s, &r, &c) == DecodeResult::OutOfRange);

    s.value_type = ValueType::Uint16;
    s.scale_exp = -1;
    s.no_reading = NoReading::XFFFF;
    r = 455;
    CHECK(decode_value(s, &r, &c) == DecodeResult::Ok);
    CHECK_NEAR(c, 45.5, 1e-4);
    r = 0xFFFF;
    CHECK(decode_value(s, &r, &c) == DecodeResult::NoReading);
    s.no_reading = NoReading::None;
    r = 0x8000;  // 32768 x 0.1 = 3276.8 C
    CHECK(decode_value(s, &r, &c) == DecodeResult::OutOfRange);

    // 21.5f = 0x41AC0000
    s.value_type = ValueType::Float32;
    s.scale_exp = 0;
    uint16_t f[2] = {0x41AC, 0x0000};
    CHECK(decode_value(s, f, &c) == DecodeResult::Ok);
    CHECK_NEAR(c, 21.5, 1e-6);
    s.value_type = ValueType::Float32Swapped;
    uint16_t fs[2] = {0x0000, 0x41AC};
    CHECK(decode_value(s, fs, &c) == DecodeResult::Ok);
    CHECK_NEAR(c, 21.5, 1e-6);
    uint16_t nan[2] = {0x7FC0, 0x0000};
    s.value_type = ValueType::Float32;
    CHECK(decode_value(s, nan, &c) == DecodeResult::NoReading);
    CHECK(register_count(ValueType::Float32) == 2);
    CHECK(register_count(ValueType::Int16) == 1);
}

static void test_blob_roundtrip_and_corruption() {
    Settings s = defaults();
    s.flow_lpm_x10 = 425;
    s.fluid = arctic::LoopFluid::PropyleneGlycol;
    s.glycol_pct = 35;
    s.sensors[0] = modbus_sensor("192.168.10.60", 103);
    s.sensors[0].rom_known = true;
    for (size_t i = 0; i < kRomLen; ++i) s.sensors[0].rom[i] = static_cast<uint8_t>(0x28 + i);
    s.sensors[1] = modbus_sensor("thermux.local", 104);
    s.sensors[1].unit_id = 7;
    s.sensors[1].reg_type = RegisterType::Holding;
    s.sensors[1].port = 1502;

    uint8_t buf[kBlobSize];
    serialize(s, buf);
    Settings out = defaults();
    CHECK(deserialize(buf, sizeof(buf), &out));
    CHECK(out.flow_lpm_x10 == 425);
    CHECK(out.fluid == arctic::LoopFluid::PropyleneGlycol);
    CHECK(out.glycol_pct == 35);
    CHECK(same_source(out.sensors[0], s.sensors[0]));
    CHECK(same_source(out.sensors[1], s.sensors[1]));
    CHECK(out.sensors[0].rom_known && out.sensors[0].rom[7] == 0x2F);
    CHECK(!out.sensors[1].rom_known);

    uint8_t bad[kBlobSize];
    std::memcpy(bad, buf, sizeof(buf));
    bad[5] ^= 1;
    Settings untouched = defaults();
    CHECK(!deserialize(bad, sizeof(bad), &untouched));
    CHECK(untouched.flow_lpm_x10 == 400);
    CHECK(!deserialize(buf, sizeof(buf) - 1, &untouched));
    std::memcpy(bad, buf, sizeof(buf));
    bad[0] = 2;  // future version
    CHECK(!deserialize(bad, sizeof(bad), &untouched));
}

static void test_nvs_persistence() {
    nvs_fake::reset();
    Settings s = load();
    CHECK(s.flow_lpm_x10 == 400);  // nothing stored

    s.flow_lpm_x10 = 380;
    s.sensors[1] = modbus_sensor("10.0.0.5", 150);
    CHECK(save(s));
    Settings back = load();
    CHECK(back.flow_lpm_x10 == 380);
    CHECK(std::strcmp(back.sensors[1].host, "10.0.0.5") == 0);

    Settings invalid = s;
    invalid.flow_lpm_x10 = 0;
    CHECK(!save(invalid));
    CHECK(load().flow_lpm_x10 == 380);

    nvs_fake::fail_next(nvs_fake::Op::SetBlob, ESP_FAIL);
    s.flow_lpm_x10 = 500;
    CHECK(!save(s));
    CHECK(load().flow_lpm_x10 == 380);

    // Garbage in NVS -> defaults, not a half-parsed struct.
    nvs_fake::seed_blob("perf", "cfg", std::vector<uint8_t>(kBlobSize, 0xAB));
    CHECK(load().flow_lpm_x10 == 400);
}

// ---- Modbus TCP framing -------------------------------------------------------

static void test_build_read() {
    uint8_t req[mbtcp::kRequestLen];
    CHECK(mbtcp::build_read(0x1234, 1, mbtcp::kFcReadInput, 103, 1, req));
    const uint8_t expect[] = {0x12, 0x34, 0, 0, 0, 6, 1, 0x04, 0, 103, 0, 1};
    CHECK(std::memcmp(req, expect, sizeof(expect)) == 0);
    CHECK(!mbtcp::build_read(1, 1, 0x06, 0, 1, req));
    CHECK(!mbtcp::build_read(1, 1, 0x03, 0, 0, req));
    CHECK(!mbtcp::build_read(1, 1, 0x03, 0, 126, req));
    CHECK(!mbtcp::build_read(1, 1, 0x03, 0xFFFF, 2, req));
}

static void test_parse_read() {
    uint16_t regs[2] = {};
    uint8_t exc = 0;
    const uint8_t ok[] = {0x12, 0x34, 0, 0, 0, 7, 1, 0x04, 4, 0x11, 0xAA, 0x80, 0x00};
    CHECK(mbtcp::frame_length(ok, 6) == sizeof(ok));
    CHECK(mbtcp::frame_length(ok, 5) == 0);
    CHECK(mbtcp::parse_read(ok, sizeof(ok), 0x1234, 1, 0x04, 2, regs, &exc) == mbtcp::Parse::Ok);
    CHECK(regs[0] == 0x11AA && regs[1] == 0x8000);
    CHECK(mbtcp::parse_read(ok, sizeof(ok) - 1, 0x1234, 1, 0x04, 2, regs, &exc) ==
          mbtcp::Parse::Incomplete);
    CHECK(mbtcp::parse_read(ok, sizeof(ok), 0x1235, 1, 0x04, 2, regs, &exc) ==
          mbtcp::Parse::WrongTransaction);
    CHECK(mbtcp::parse_read(ok, sizeof(ok), 0x1234, 2, 0x04, 2, regs, &exc) ==
          mbtcp::Parse::WrongUnit);
    CHECK(mbtcp::parse_read(ok, sizeof(ok), 0x1234, 1, 0x03, 2, regs, &exc) ==
          mbtcp::Parse::WrongFunction);
    CHECK(mbtcp::parse_read(ok, sizeof(ok), 0x1234, 1, 0x04, 1, regs, &exc) ==
          mbtcp::Parse::BadLength);

    const uint8_t ex[] = {0x12, 0x34, 0, 0, 0, 3, 1, 0x84, 0x02};
    CHECK(mbtcp::parse_read(ex, sizeof(ex), 0x1234, 1, 0x04, 1, regs, &exc) ==
          mbtcp::Parse::Exception);
    CHECK(exc == 2);

    const uint8_t proto[] = {0x12, 0x34, 0, 1, 0, 5, 1, 0x04, 2, 0, 0};
    CHECK(mbtcp::parse_read(proto, sizeof(proto), 0x1234, 1, 0x04, 1, regs, &exc) ==
          mbtcp::Parse::BadProtocol);
    const uint8_t huge[] = {0x12, 0x34, 0, 0, 0xFF, 0xFF, 1};
    CHECK(mbtcp::frame_length(huge, sizeof(huge)) == 0);
    CHECK(mbtcp::parse_read(huge, sizeof(huge), 0x1234, 1, 0x04, 1, regs, &exc) ==
          mbtcp::Parse::BadLength);
}

// ---- median window and source selection --------------------------------------

static void test_median_window() {
    MedianWindow w;
    w.add(0, 30.0f);
    w.add(10000, 99.0f);  // spike
    w.add(20000, 31.0f);
    CHECK(w.size() == 3);
    CHECK_NEAR(w.median(), 31.0, 1e-6);
    w.add(30000, 32.0f);
    CHECK_NEAR(w.median(), 31.5, 1e-6);
    w.prune(125000);  // drops t=0 (older than 120 s)
    CHECK(w.size() == 3);
    w.prune(1000000);
    CHECK(w.size() == 0);
    for (uint32_t i = 0; i < 40; ++i) w.add(i, 1.0f);
    CHECK(w.size() == kWindowCapacity);
}

static const HeatPumpContext kRunningHeat{true, false, arctic::MaconMode::Heating};
static const HeatPumpContext kIdleHeat{false, false, arctic::MaconMode::Heating};

static void feed(SourceSelector& sel, uint32_t from, uint32_t to, float supply, float ret,
                 const bool configured[2], const HeatPumpContext& hp, Selection* last) {
    for (uint32_t t = from; t <= to; t += 10000) {
        if (configured[0]) sel.on_reading(Slot::Supply, t, supply);
        if (configured[1]) sel.on_reading(Slot::Return, t, ret);
        *last = sel.evaluate(t, configured, hp);
    }
}

static void test_selector_not_configured() {
    SourceSelector sel;
    bool none[2] = {false, false};
    Selection s = sel.evaluate(0, none, kRunningHeat);
    CHECK(s.source == PerfSource::HeatPump);
    CHECK(!s.fallback);
    CHECK(!s.settling);
}

static void test_selector_needs_a_clean_minute_before_using_external() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 50000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::HeatPump);
    // Readings are fine, just not trusted yet: not a failure.
    CHECK(!s.fallback);
    CHECK(s.pending);
    feed(sel, 60000, 60000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);
    CHECK(!s.fallback);
    CHECK(!s.pending);
    CHECK_NEAR(s.supply_c, 35.0, 1e-6);
    CHECK_NEAR(s.return_c, 30.0, 1e-6);
}

static void test_selector_falls_back_and_recovers_pairwise() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 120000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);

    // Return sensor errors once: both fall back at once.
    sel.on_reading(Slot::Supply, 130000, 35.0f);
    sel.on_error(Slot::Return, 130000);
    s = sel.evaluate(130000, both, kIdleHeat);
    CHECK(s.source == PerfSource::HeatPump);
    CHECK(s.fallback);
    CHECK(!s.pending);

    // Good again, but must stay good for a minute first.
    feed(sel, 140000, 190000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::HeatPump);
    CHECK(!s.fallback);
    CHECK(s.pending);
    feed(sel, 200000, 200000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);

    // Silence (worker stalled, no errors reported) also falls back once stale.
    s = sel.evaluate(200000 + kStaleMs, both, kIdleHeat);
    CHECK(s.source == PerfSource::External);
    s = sel.evaluate(200000 + kStaleMs + 1, both, kIdleHeat);
    CHECK(s.source == PerfSource::HeatPump);
    CHECK(s.fallback);
}

static void test_selector_never_read_error_is_a_fallback() {
    SourceSelector sel;
    bool both[2] = {true, true};
    sel.on_reading(Slot::Supply, 0, 35.0f);
    sel.on_error(Slot::Return, 0);
    Selection s = sel.evaluate(0, both, kIdleHeat);
    CHECK(s.source == PerfSource::HeatPump);
    CHECK(s.fallback);
    CHECK(!s.pending);
}

static void test_selector_settles_after_start_defrost_and_mode_change() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 100000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External && !s.settling);

    feed(sel, 110000, 110000, 35.0f, 30.0f, both, kRunningHeat, &s);  // compressor starts
    CHECK(s.settling);
    feed(sel, 120000, 110000 + kSettleMs - 10000, 35.0f, 30.0f, both, kRunningHeat, &s);
    CHECK(s.settling);
    feed(sel, 110000 + kSettleMs, 110000 + kSettleMs, 35.0f, 30.0f, both, kRunningHeat, &s);
    CHECK(!s.settling);

    uint32_t t = 110000 + kSettleMs + 10000;
    HeatPumpContext defrost = kRunningHeat;
    defrost.defrost = true;
    feed(sel, t, t, 35.0f, 30.0f, both, defrost, &s);
    CHECK(!s.settling);  // the estimate itself is invalid during defrost
    feed(sel, t + 10000, t + 10000, 35.0f, 30.0f, both, kRunningHeat, &s);  // defrost ends
    CHECK(s.settling);

    t += 10000;
    feed(sel, t, t + kSettleMs, 35.0f, 30.0f, both, kRunningHeat, &s);
    CHECK(s.source == PerfSource::External);
    CHECK(!s.settling);
    t += kSettleMs;
    HeatPumpContext cooling{true, false, arctic::MaconMode::Cooling};
    feed(sel, t + 10000, t + 10000, 25.0f, 30.0f, both, cooling, &s);
    CHECK(s.settling);
}

static void test_selector_recovery_restarts_after_silence() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 120000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);
    // Nothing for five minutes, then readings resume: a fresh clean minute is
    // needed, exactly as after an error.
    s = sel.evaluate(420000, both, kIdleHeat);
    CHECK(s.source == PerfSource::HeatPump);
    feed(sel, 430000, 480000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::HeatPump);
    feed(sel, 490000, 490000, 35.0f, 30.0f, both, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);
}

static void test_selector_running_at_first_sight_settles() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 60000, 35.0f, 30.0f, both, kRunningHeat, &s);
    CHECK(s.source == PerfSource::External);
    CHECK(s.settling);
    feed(sel, 70000, kSettleMs, 35.0f, 30.0f, both, kRunningHeat, &s);
    CHECK(!s.settling);
}

static void test_selector_single_external_sensor() {
    SourceSelector sel;
    bool supply_only[2] = {true, false};
    Selection s{};
    feed(sel, 0, 60000, 35.5f, 0.0f, supply_only, kIdleHeat, &s);
    CHECK(s.source == PerfSource::External);
    CHECK_NEAR(s.supply_c, 35.5, 1e-6);
}

static void test_selector_median_rejects_spike() {
    SourceSelector sel;
    bool both[2] = {true, true};
    Selection s{};
    feed(sel, 0, 60000, 35.0f, 30.0f, both, kIdleHeat, &s);
    sel.on_reading(Slot::Supply, 70000, 85.0f);
    sel.on_reading(Slot::Return, 70000, 30.0f);
    s = sel.evaluate(70000, both, kIdleHeat);
    CHECK_NEAR(s.supply_c, 35.0, 1e-6);
}

// ---- Thermux ------------------------------------------------------------------

static void test_thermux() {
    SensorConfig s = modbus_sensor("thermux.local", 103);
    CHECK(thermux::candidate(s));
    CHECK(thermux::channel_of(s) == 3);
    s.address = 200;
    CHECK(!thermux::candidate(s));
    s.address = 103;
    s.reg_type = RegisterType::Holding;
    CHECK(!thermux::candidate(s));

    uint16_t info[thermux::kInfoCount] = {};
    info[0] = 1;
    info[4] = 100;
    info[15] = 10;
    CHECK(thermux::info_matches(info));
    info[15] = 0;
    CHECK(!thermux::info_matches(info));
    info[15] = 10;
    info[0] = 2;
    CHECK(!thermux::info_matches(info));

    uint16_t rom_regs[4] = {0x28FF, 0x1234, 0x5678, 0x9ABC};
    uint8_t rom[kRomLen];
    thermux::rom_from_regs(rom_regs, rom);
    const uint8_t expect[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    CHECK(std::memcmp(rom, expect, kRomLen) == 0);
}

int main() {
    struct {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"defaults_are_valid", test_defaults_are_valid},
        {"validation", test_validation},
        {"host_valid", test_host_valid},
        {"names_roundtrip", test_names_roundtrip},
        {"decode", test_decode},
        {"blob_roundtrip_and_corruption", test_blob_roundtrip_and_corruption},
        {"nvs_persistence", test_nvs_persistence},
        {"build_read", test_build_read},
        {"parse_read", test_parse_read},
        {"median_window", test_median_window},
        {"selector_not_configured", test_selector_not_configured},
        {"selector_needs_a_clean_minute", test_selector_needs_a_clean_minute_before_using_external},
        {"selector_falls_back_and_recovers", test_selector_falls_back_and_recovers_pairwise},
        {"selector_never_read_error_is_a_fallback", test_selector_never_read_error_is_a_fallback},
        {"selector_settles", test_selector_settles_after_start_defrost_and_mode_change},
        {"selector_recovery_restarts_after_silence", test_selector_recovery_restarts_after_silence},
        {"selector_running_at_first_sight", test_selector_running_at_first_sight_settles},
        {"selector_single_external_sensor", test_selector_single_external_sensor},
        {"selector_median_rejects_spike", test_selector_median_rejects_spike},
        {"thermux", test_thermux},
    };
    for (const auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
