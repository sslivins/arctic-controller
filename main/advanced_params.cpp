/*
 * Arctic Heat Pump Controller
 * Advanced ("AP") technician-parameter access - Implementation
 */
#include "advanced_params.h"
#include "app_preferences.h"
#include "event_log.h"
#include "heatpump_controller.h"
#include <esp_log.h>
#include <atomic>

static const char* TAG = "adv_params";

using arctic::AdvWriteResult;
using arctic::AdvWritePlan;
using arctic::AdvancedParam;

// Demo mode has no mainboard to hold written values, so keep them here (RAM
// only, like the demo setpoints) and answer reads from them; otherwise every
// save would snap back to the default on the next read.
namespace {
struct DemoValue { uint8_t ap; uint16_t raw; };
constexpr int kDemoSlots = 64;
DemoValue s_demo[kDemoSlots];
int s_demo_count = 0;
std::atomic<uint32_t> s_change_count{0};

const DemoValue* demo_find(uint8_t ap) {
    for (int i = 0; i < s_demo_count; i++) if (s_demo[i].ap == ap) return &s_demo[i];
    return nullptr;
}

void demo_store(uint8_t ap, uint16_t raw) {
    for (int i = 0; i < s_demo_count; i++) {
        if (s_demo[i].ap == ap) { s_demo[i].raw = raw; return; }
    }
    if (s_demo_count < kDemoSlots) s_demo[s_demo_count++] = {ap, raw};
}

// The value as the API reports it: the display value, or the option id for a
// choice. EVENT_PARAM_UNKNOWN when it can't be read.
int client_value(uint8_t ap) {
    int16_t v = 0;
    if (!advanced_param_read(ap, &v)) return EVENT_PARAM_UNKNOWN;
    if (arctic::advanced_enum_option_count(ap) > 0)
        return arctic::advanced_enum_option_index_for_wire(ap, v);
    return v;
}

// Logged so a technician can see, and put back, what a setting was before.
void log_change(uint8_t ap, int from, int to) {
    if (from == to) return;
    s_change_count.fetch_add(1, std::memory_order_relaxed);
    event_log_record(EVENT_PARAM_CHANGED, event_param_changed_pack(ap, from, to));
}
}  // namespace

uint32_t advanced_param_change_count(void) {
    return s_change_count.load(std::memory_order_relaxed);
}

bool advanced_param_read(uint8_t ap, int16_t* value_out) {
    if (!value_out) return false;

    const AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (!p) {
        ESP_LOGW(TAG, "read AP%u: unknown parameter", (unsigned)ap);
        return false;
    }
    if (p->reg == arctic::ADV_REG_UNKNOWN) {
        // Register not change-and-capture verified: refuse rather than read a
        // guessed register and show a bogus value.
        ESP_LOGD(TAG, "read AP%u (%s): register unverified", (unsigned)ap, p->name);
        return false;
    }

    // Demo mode - no live bus; report the last demo write, else the vendor-doc default.
    if (app_prefs_is_demo_mode()) {
        const DemoValue* d = demo_find(ap);
        *value_out = arctic::advanced_display_value(
            ap, d ? arctic::advanced_decode_raw(ap, d->raw) : p->default_val);
        return true;
    }

    if (!arctic::isConnected()) {
        ESP_LOGW(TAG, "read AP%u: not connected", (unsigned)ap);
        return false;
    }

    uint16_t raw = 0;
    if (!arctic::readRegister(p->reg, &raw)) {
        ESP_LOGW(TAG, "read AP%u: bus read of reg%u failed", (unsigned)ap, (unsigned)p->reg);
        return false;
    }
    // Signed params are 8-bit two's-complement in the low byte; the library
    // decoder sign-extends them (e.g. raw 0x00F6 -> -10), others pass through.
    *value_out = arctic::advanced_display_value(
        ap, arctic::advanced_decode_raw(ap, raw));
    return true;
}

arctic::AdvWriteResult advanced_param_write(uint8_t ap, int16_t value, bool* bus_ok) {
    if (bus_ok) *bus_ok = false;

    // Build a validated write plan (range/enum + register-known + unlocked).
    AdvWritePlan plan{};
    AdvWriteResult r = arctic::advanced_prepare_display_write(ap, value, &plan);
    if (r != AdvWriteResult::OK) {
        ESP_LOGW(TAG, "write AP%u = %d refused: %s",
                 (unsigned)ap, value, arctic::adv_write_result_name(r));
        return r;
    }

    const int from = client_value(ap);

    // Demo mode - accept the (valid) write without touching the bus.
    if (app_prefs_is_demo_mode()) {
        ESP_LOGI(TAG, "[DEMO] AP%u = %d (reg%u=%u, not sent)",
                 (unsigned)ap, value, (unsigned)plan.reg, (unsigned)plan.raw);
        demo_store(ap, plan.raw);
        log_change(ap, from, value);
        if (bus_ok) *bus_ok = true;
        return AdvWriteResult::OK;
    }

    if (!arctic::isConnected()) {
        ESP_LOGW(TAG, "write AP%u: not connected", (unsigned)ap);
        return AdvWriteResult::OK;  // guardrail passed; caller checks *bus_ok
    }

    bool ok = arctic::writeRegister(plan.reg, plan.raw);
    if (bus_ok) *bus_ok = ok;
    if (ok) {
        ESP_LOGI(TAG, "Wrote AP%u = %d (reg%u = %u)",
                 (unsigned)ap, value, (unsigned)plan.reg, (unsigned)plan.raw);
        log_change(ap, from, value);
    } else {
        ESP_LOGE(TAG, "AP%u guardrail OK but bus write of reg%u failed",
                 (unsigned)ap, (unsigned)plan.reg);
    }
    return AdvWriteResult::OK;
}

arctic::AdvWriteResult advanced_param_write_option(uint8_t ap, size_t option_index,
                                                   bool* bus_ok) {
    if (bus_ok) *bus_ok = false;

    // Build a validated write plan by opaque option id: arctic-macon maps the
    // id to its wire code and runs the range/enum + register-known + unlocked
    // guardrail. The raw wire code stays inside the library/this module.
    AdvWritePlan plan{};
    AdvWriteResult r = arctic::advanced_prepare_write_option(ap, option_index, &plan);
    if (r != AdvWriteResult::OK) {
        ESP_LOGW(TAG, "write AP%u option %u refused: %s",
                 (unsigned)ap, (unsigned)option_index, arctic::adv_write_result_name(r));
        return r;
    }

    const int from = client_value(ap);

    // Demo mode - accept the (valid) write without touching the bus.
    if (app_prefs_is_demo_mode()) {
        ESP_LOGI(TAG, "[DEMO] AP%u option %u (reg%u=%u, not sent)",
                 (unsigned)ap, (unsigned)option_index, (unsigned)plan.reg, (unsigned)plan.raw);
        demo_store(ap, plan.raw);
        log_change(ap, from, (int)option_index);
        if (bus_ok) *bus_ok = true;
        return AdvWriteResult::OK;
    }

    if (!arctic::isConnected()) {
        ESP_LOGW(TAG, "write AP%u option: not connected", (unsigned)ap);
        return AdvWriteResult::OK;  // guardrail passed; caller checks *bus_ok
    }

    bool ok = arctic::writeRegister(plan.reg, plan.raw);
    if (bus_ok) *bus_ok = ok;
    if (ok) {
        ESP_LOGI(TAG, "Wrote AP%u option %u (reg%u = %u)",
                 (unsigned)ap, (unsigned)option_index, (unsigned)plan.reg, (unsigned)plan.raw);
        log_change(ap, from, (int)option_index);
    } else {
        ESP_LOGE(TAG, "AP%u option guardrail OK but bus write of reg%u failed",
                 (unsigned)ap, (unsigned)plan.reg);
    }
    return AdvWriteResult::OK;
}
