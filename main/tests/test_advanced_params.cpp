// Native unit tests for main/advanced_params.cpp -- the layer between the UI /
// API and the technician ("AP") parameter registers on the heat pump's
// mainboard.
//
// These parameters are the dangerous ones. They are the vendor's internal
// service settings: compressor frequency ratios, defrost thresholds, sensor
// offsets. A bad write here is not a wrong number on a screen, it is a heat
// pump running outside the envelope its manufacturer designed for, and there is
// no undo.
//
// What is real and what is faked, and why. The parameter table, the range and
// enum guardrail, and the wire encoding are the actual arctic-macon code,
// linked as-is and already covered by that component's own suite. The only
// thing faked is the bus itself -- isConnected / readRegister / writeRegister --
// because that is an RS485 transaction with physical hardware. Faking the
// guardrail instead would mean asserting that a stand-in for the logic under
// test agrees with itself.
//
// What this file is actually for. main/advanced_params.cpp is thin, and thin
// wrappers are where ordering mistakes hide:
//
//   * demo mode must not bypass the guardrail. The demo short-circuit sits
//     AFTER validation on purpose, so a device in demo mode cannot be used to
//     wave an out-of-range value through. Move those two blocks and every
//     "does demo mode work" test still passes.
//   * an unverified register must be refused even in demo mode, because the
//     refusal is about not showing a value read from a guessed register. The
//     current table has none, so that branch is unreachable today and is
//     covered here by the table invariant that keeps it that way.
//   * "not connected" returns OK with *bus_ok false. That is a genuinely
//     awkward contract -- the guardrail passed, nothing was sent -- and a
//     caller that checks only the return value reports success for a write
//     that never happened.
//
// Parameters are discovered from the live table rather than hard-coded by
// number, so this suite tests the wrapper's behaviour rather than duplicating
// arctic-macon's data. Each finder fails loudly when the table no longer
// contains a parameter of the shape a scenario needs, so a table change cannot
// quietly turn these tests into no-ops.
//
// Refs #217 (T13).

#include "bus_fake.h"
#include "nvs_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "advanced_params.h"
#include "app_preferences.h"
#include "macon_advanced_params.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                      \
    do {                                                                    \
        long a_ = (long)(actual);                                           \
        long e_ = (long)(expected);                                         \
        if (a_ != e_) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %ld, expected %ld\n", \
                         __FILE__, __LINE__, #actual, a_, e_);              \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

using arctic::AdvancedParam;
using arctic::AdvWritePlan;
using arctic::AdvWriteResult;

constexpr uint8_t AP_MAX = 200;

// A parameter that can actually be written: register verified, not locked, not
// read-only, and a plain numeric range rather than an enum.
const AdvancedParam* find_writable() {
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (!p) continue;
        if (p->reg == arctic::ADV_REG_UNKNOWN) continue;
        if (p->needs_sim_confirm || p->read_only || p->is_trigger) continue;
        if (p->enum_vals != nullptr) continue;
        if (p->min_val >= p->max_val) continue;
        if (p->display_multiplier != 1) continue;
        if (p->is_signed) continue;
        return p;
    }
    return nullptr;
}

// A signed parameter with a verified register: the two's-complement decode path.
const AdvancedParam* find_signed_readable() {
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (p && p->is_signed && p->reg != arctic::ADV_REG_UNKNOWN) return p;
    }
    return nullptr;
}

// An enum parameter with a verified register and at least two options.
const AdvancedParam* find_enum_writable() {
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (!p) continue;
        if (p->reg == arctic::ADV_REG_UNKNOWN) continue;
        if (p->needs_sim_confirm || p->read_only) continue;
        if (p->enum_vals == nullptr || p->enum_count < 2) continue;
        return p;
    }
    return nullptr;
}

// A parameter whose display value differs from its wire value: display_multiplier
// greater than one. Without one of these, "the display conversion is skipped"
// and "the display value is written instead of the encoded raw" are both
// invisible, because raw == display for every identity-scaled parameter.
const AdvancedParam* find_scaled_writable() {
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (!p) continue;
        if (p->reg == arctic::ADV_REG_UNKNOWN) continue;
        if (p->needs_sim_confirm || p->read_only || p->is_trigger) continue;
        if (p->enum_vals != nullptr) continue;
        if (p->display_multiplier <= 1) continue;
        if (p->min_val >= p->max_val) continue;
        return p;
    }
    return nullptr;
}

// An enum parameter whose wire codes are NOT 0,1,2,... so that the option id
// (a list index) and the code that crosses the bus are distinguishable. With an
// identity enum, writing the index instead of the code is a no-op.
const AdvancedParam* find_nonidentity_enum() {
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (!p) continue;
        if (p->reg == arctic::ADV_REG_UNKNOWN) continue;
        if (p->needs_sim_confirm || p->read_only) continue;
        if (p->enum_vals == nullptr || p->enum_count < 2) continue;
        for (uint8_t i = 0; i < p->enum_count; ++i) {
            if (p->enum_vals[i] != i) return p;
        }
    }
    return nullptr;
}

// An unused AP number.
uint8_t find_unknown_ap() {
    for (uint8_t ap = AP_MAX; ap > 0; --ap) {
        if (!arctic::advanced_param_lookup(ap)) return ap;
    }
    return 0;
}

// A scenario whose fixture has vanished from the table must fail, not pass
// vacuously.
#define REQUIRE_PARAM(p, what)                                              \
    do {                                                                    \
        if (!(p)) {                                                         \
            std::fprintf(stderr,                                            \
                         "  FAIL %s:%d: the parameter table no longer has " \
                         "%s, so this scenario tests nothing\n",            \
                         __FILE__, __LINE__, what);                         \
            ++g_failures;                                                   \
            return;                                                         \
        }                                                                   \
    } while (0)

void live_device() {
    nvs_fake::reset();
    bus_fake::reset();
    app_prefs_init();
    bus_fake::set_connected(true);
}

void demo_device() {
    nvs_fake::reset();
    bus_fake::reset();
    app_prefs_init();
    app_prefs_set_demo_mode(true);
    // Deliberately still "connected": every demo assertion below is then about
    // the demo branch being taken, not about there being no bus to talk to.
    bus_fake::set_connected(true);
}

// ------------------------------------------------------------------------
// Reading
// ------------------------------------------------------------------------
void an_unknown_parameter_cannot_be_read() {
    live_device();
    uint8_t ap = find_unknown_ap();
    CHECK(ap != 0);

    int16_t value = 1234;
    CHECK(advanced_param_read(ap, &value) == false);
    CHECK_EQ_INT(bus_fake::reads().size(), 0);
    CHECK_EQ_INT(value, 1234);  // left alone
}

void a_null_destination_is_refused() {
    live_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    CHECK(advanced_param_read(p->ap, nullptr) == false);
    CHECK_EQ_INT(bus_fake::reads().size(), 0);
}

void every_parameter_has_a_verified_register() {
    live_device();

    // The current table has no unverified registers at all, which makes the
    // ADV_REG_UNKNOWN refusal in advanced_param_read() unreachable today. That
    // is worth stating rather than quietly leaving three scenarios that pass
    // by testing nothing.
    //
    // The refusal is still the right defence: a register that was only inferred
    // by correlation would otherwise be read and its value shown to a
    // technician as though it were authoritative. So this scenario asserts the
    // property that keeps the branch dead. The day someone adds an AP with
    // reg = 0, this fails, and whoever is looking at it is pointed straight at
    // the path that now needs covering.
    int checked = 0;
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (!p) continue;
        ++checked;
        if (p->reg == arctic::ADV_REG_UNKNOWN) {
            std::fprintf(stderr,
                         "  FAIL %s:%d: AP%u (%s) has an unverified register. "
                         "The refusal paths in advanced_param_read/write are "
                         "now reachable and need their own scenarios.\n",
                         __FILE__, __LINE__, (unsigned)p->ap, p->name);
            ++g_failures;
        }
    }
    // And that the scan itself is not vacuous.
    CHECK(checked > 20);
}

void a_disconnected_bus_fails_the_read() {
    live_device();
    bus_fake::set_connected(false);
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == false);
    CHECK_EQ_INT(bus_fake::reads().size(), 0);
}

void a_failed_bus_read_is_reported() {
    live_device();
    bus_fake::set_read_ok(false);
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    int16_t value = 7;
    CHECK(advanced_param_read(p->ap, &value) == false);
    // It did try -- the failure is the mainboard's, not a refusal to ask.
    CHECK_EQ_INT(bus_fake::reads().size(), 1);
    CHECK_EQ_INT(value, 7);
}

void a_successful_read_uses_the_verified_register() {
    live_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");
    bus_fake::set_read_value((uint16_t)p->min_val);

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == true);
    CHECK_EQ_INT(bus_fake::reads().size(), 1);
    // The confirmed register from the table, not the AP number and not a
    // computed 2000 + ap guess.
    CHECK_EQ_INT(bus_fake::reads()[0], p->reg);
    CHECK_EQ_INT(value, p->min_val);
}

void a_signed_value_is_sign_extended() {
    live_device();
    const AdvancedParam* p = find_signed_readable();
    REQUIRE_PARAM(p, "a signed parameter with a verified register");

    // The mainboard stores signed params as 8-bit two's complement in the low
    // byte. Read as a plain uint16 this is 246, which as a sensor offset is
    // nonsense the UI would happily display.
    bus_fake::set_read_value(0x00F6);

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == true);
    CHECK_EQ_INT(value, arctic::advanced_display_value(p->ap, -10));
}

void a_scaled_parameter_is_read_in_display_units() {
    live_device();
    const AdvancedParam* p = find_scaled_writable();
    REQUIRE_PARAM(p, "a parameter with a display multiplier above one");

    // The wire holds the value in its own units and the multiplier converts it
    // for the screen. Stating the expectation as an independent arithmetic
    // product rather than as advanced_display_value(p->ap, 5) matters: the
    // latter would agree with any multiplier the library happened to apply,
    // including none.
    const int16_t wire = 5;
    const long expected = (long)wire * (long)p->display_multiplier;

    bus_fake::set_read_value((uint16_t)wire);

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == true);
    CHECK_EQ_INT(value, expected);
    CHECK(value != wire);  // the scenario is only meaningful if these differ
}

void a_scaled_demo_read_reports_display_units() {
    demo_device();
    const AdvancedParam* p = find_scaled_writable();
    REQUIRE_PARAM(p, "a parameter with a display multiplier above one");

    // Demo mode answers from the vendor-doc default, which is stored in wire
    // units like everything else in the table. Reporting it unconverted makes
    // demo mode disagree with a live device by exactly the multiplier -- a
    // difference nobody would question, because both numbers look plausible.
    const long expected = (long)p->default_val * (long)p->display_multiplier;

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == true);
    CHECK_EQ_INT(value, expected);
    CHECK(value != p->default_val);
}

void a_scaled_write_encodes_back_to_wire_units() {
    live_device();
    const AdvancedParam* p = find_scaled_writable();
    REQUIRE_PARAM(p, "a parameter with a display multiplier above one");

    // The technician types a display-unit number; the mainboard must receive
    // the wire-unit one. Writing the display value straight through would
    // multiply the real setting by the multiplier -- here, turning a 30-unit
    // wait into a 300-unit one -- and the write would report success.
    const int16_t wire = 3;
    const int16_t display = (int16_t)(wire * p->display_multiplier);

    bool bus_ok = false;
    CHECK(advanced_param_write(p->ap, display, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
    if (bus_fake::writes().size() == 1) {
        CHECK_EQ_INT(bus_fake::writes()[0].reg, p->reg);
        CHECK_EQ_INT(bus_fake::writes()[0].raw, wire);
        CHECK(bus_fake::writes()[0].raw != (uint16_t)display);
    }
}

void an_option_write_sends_the_wire_code_not_the_index() {
    live_device();
    const AdvancedParam* p = find_nonidentity_enum();
    REQUIRE_PARAM(p, "an enum parameter whose wire codes are not 0,1,2,...");

    // Find an option whose list position differs from its code. On these
    // parameters the codes are sparse (0,1,2,4,8,...), so the id and the code
    // diverge from the fourth entry on. Writing the id would silently select a
    // different, still-valid setting.
    size_t idx = 0;
    bool found = false;
    for (uint8_t i = 0; i < p->enum_count; ++i) {
        if (p->enum_vals[i] != i) { idx = i; found = true; break; }
    }
    CHECK(found == true);
    if (!found) return;
    const uint16_t code = p->enum_vals[idx];

    bool bus_ok = false;
    CHECK(advanced_param_write_option(p->ap, idx, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
    if (bus_fake::writes().size() == 1) {
        CHECK_EQ_INT(bus_fake::writes()[0].reg, p->reg);
        CHECK_EQ_INT(bus_fake::writes()[0].raw, code);
        CHECK(bus_fake::writes()[0].raw != (uint16_t)idx);
    }
}

void demo_mode_reports_the_documented_default() {
    demo_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    int16_t value = 0;
    CHECK(advanced_param_read(p->ap, &value) == true);
    CHECK_EQ_INT(value, arctic::advanced_display_value(p->ap, p->default_val));
    // And it must not have gone near the bus: in demo mode there may not be
    // one, and a demo session must never generate traffic to a real mainboard.
    CHECK_EQ_INT(bus_fake::reads().size(), 0);
}

// ------------------------------------------------------------------------
// Writing a value
// ------------------------------------------------------------------------
void an_unknown_parameter_cannot_be_written() {
    live_device();
    uint8_t ap = find_unknown_ap();
    CHECK(ap != 0);

    bool bus_ok = true;
    CHECK(advanced_param_write(ap, 1, &bus_ok) == AdvWriteResult::UNKNOWN_PARAM);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void an_out_of_range_value_is_refused() {
    live_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    bool bus_ok = true;
    AdvWriteResult r = advanced_param_write(p->ap, (int16_t)(p->max_val + 1), &bus_ok);
    CHECK(r == AdvWriteResult::OUT_OF_RANGE);
    CHECK(bus_ok == false);
    // Refused, never clamped: silently writing the nearest legal value would
    // leave the technician believing a different setting took effect.
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void an_out_of_range_value_is_refused_in_demo_mode_too() {
    demo_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    // The load-bearing ordering assertion in this file. The demo short-circuit
    // sits after the guardrail; if the two were swapped, demo mode would
    // cheerfully accept values the hardware must never be given, and the UI
    // built against demo mode would look correct right up until it was pointed
    // at a real unit.
    bool bus_ok = false;
    AdvWriteResult r = advanced_param_write(p->ap, (int16_t)(p->max_val + 1), &bus_ok);
    CHECK(r == AdvWriteResult::OUT_OF_RANGE);
    CHECK(bus_ok == false);
}

void a_valid_write_reaches_the_wire() {
    live_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    AdvWritePlan expected{};
    CHECK(arctic::advanced_prepare_display_write(p->ap, p->max_val, &expected) ==
          AdvWriteResult::OK);

    bool bus_ok = false;
    CHECK(advanced_param_write(p->ap, p->max_val, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
    CHECK_EQ_INT(bus_fake::writes()[0].reg, expected.reg);
    CHECK_EQ_INT(bus_fake::writes()[0].raw, expected.raw);
}

void a_disconnected_write_reports_ok_but_not_sent() {
    live_device();
    bus_fake::set_connected(false);
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    // The awkward contract, pinned down. OK here means "the guardrail passed",
    // NOT "the setting was applied". Callers must consult bus_ok. Asserting it
    // explicitly means anyone who decides to change it has to change this test
    // and read the reason.
    bool bus_ok = true;
    CHECK(advanced_param_write(p->ap, p->max_val, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void a_rejected_bus_write_is_reported() {
    live_device();
    bus_fake::set_write_ok(false);
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    bool bus_ok = true;
    CHECK(advanced_param_write(p->ap, p->max_val, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
}

void demo_mode_accepts_a_valid_write_without_touching_the_bus() {
    demo_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    bool bus_ok = false;
    CHECK(advanced_param_write(p->ap, p->max_val, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void a_null_bus_ok_is_tolerated() {
    live_device();
    const AdvancedParam* p = find_writable();
    REQUIRE_PARAM(p, "a plainly writable parameter");

    // Callers that only care whether the guardrail passed pass nullptr.
    CHECK(advanced_param_write(p->ap, p->max_val, nullptr) == AdvWriteResult::OK);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
}

// ------------------------------------------------------------------------
// Writing an enum option by index
// ------------------------------------------------------------------------
void an_unknown_option_is_refused() {
    live_device();
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    bool bus_ok = true;
    AdvWriteResult r = advanced_param_write_option(p->ap, p->enum_count + 5, &bus_ok);
    CHECK(r != AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void an_option_write_puts_the_wire_code_on_the_bus() {
    live_device();
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    AdvWritePlan expected{};
    CHECK(arctic::advanced_prepare_write_option(p->ap, 1, &expected) ==
          AdvWriteResult::OK);

    bool bus_ok = false;
    CHECK(advanced_param_write_option(p->ap, 1, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
    CHECK_EQ_INT(bus_fake::writes()[0].reg, expected.reg);
    // The option index is a UI concern; what reaches the mainboard is the
    // vendor's wire code, which is not generally equal to the index.
    CHECK_EQ_INT(bus_fake::writes()[0].raw, expected.raw);
}

void a_disconnected_option_write_reports_ok_but_not_sent() {
    live_device();
    bus_fake::set_connected(false);
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    bool bus_ok = true;
    CHECK(advanced_param_write_option(p->ap, 1, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void a_rejected_option_bus_write_is_reported() {
    live_device();
    bus_fake::set_write_ok(false);
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    bool bus_ok = true;
    CHECK(advanced_param_write_option(p->ap, 1, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
}

void demo_mode_accepts_a_valid_option_without_touching_the_bus() {
    demo_device();
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    bool bus_ok = false;
    CHECK(advanced_param_write_option(p->ap, 1, &bus_ok) == AdvWriteResult::OK);
    CHECK(bus_ok == true);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void an_invalid_option_is_refused_in_demo_mode_too() {
    demo_device();
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    bool bus_ok = false;
    AdvWriteResult r = advanced_param_write_option(p->ap, p->enum_count + 5, &bus_ok);
    CHECK(r != AdvWriteResult::OK);
    CHECK(bus_ok == false);
}

void a_null_bus_ok_is_tolerated_for_options() {
    live_device();
    const AdvancedParam* p = find_enum_writable();
    REQUIRE_PARAM(p, "a writable enum parameter");

    CHECK(advanced_param_write_option(p->ap, 1, nullptr) == AdvWriteResult::OK);
    CHECK_EQ_INT(bus_fake::writes().size(), 1);
}

// ------------------------------------------------------------------------
// Read-only and locked parameters
// ------------------------------------------------------------------------
void a_read_only_parameter_can_be_read_but_not_written() {
    live_device();
    const AdvancedParam* found = nullptr;
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (p && p->read_only && p->reg != arctic::ADV_REG_UNKNOWN) {
            found = p;
            break;
        }
    }
    REQUIRE_PARAM(found, "a read-only parameter with a verified register");

    bus_fake::set_read_value(0);
    int16_t value = 0;
    CHECK(advanced_param_read(found->ap, &value) == true);

    bool bus_ok = true;
    CHECK(advanced_param_write(found->ap, 0, &bus_ok) != AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

void a_locked_parameter_can_be_read_but_not_written() {
    live_device();
    const AdvancedParam* found = nullptr;
    for (uint8_t ap = 0; ap < AP_MAX; ++ap) {
        const AdvancedParam* p = arctic::advanced_param_lookup(ap);
        if (p && p->needs_sim_confirm && p->reg != arctic::ADV_REG_UNKNOWN &&
            !p->read_only) {
            found = p;
            break;
        }
    }
    REQUIRE_PARAM(found, "a write-locked parameter with a verified register");

    // Locked means the register is known and safe to observe, but its write
    // behaviour has not been confirmed against a real mainboard. Reads are
    // therefore fine and writes must not happen.
    bus_fake::set_read_value(0);
    int16_t value = 0;
    CHECK(advanced_param_read(found->ap, &value) == true);

    bool bus_ok = true;
    CHECK(advanced_param_write(found->ap, found->default_val, &bus_ok) !=
          AdvWriteResult::OK);
    CHECK(bus_ok == false);
    CHECK_EQ_INT(bus_fake::writes().size(), 0);
}

int run_scenario(const char* name, void (*body)()) {
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        g_failures = 0;
        body();
        std::fflush(nullptr);
        _exit(g_failures == 0 ? 0 : 1);
    }
    if (pid < 0) {
        std::fprintf(stderr, "FAIL [%s]: fork failed\n", name);
        return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "FAIL [%s]: crashed with signal %d\n", name,
                     WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "FAIL [%s]\n", name);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define SCENARIO(name, fn)                \
    do {                                  \
        ++total;                          \
        failed += run_scenario(name, fn); \
    } while (0)

    SCENARIO("an unknown parameter cannot be read",
             an_unknown_parameter_cannot_be_read);
    SCENARIO("a null destination is refused", a_null_destination_is_refused);
    SCENARIO("every parameter has a verified register",
             every_parameter_has_a_verified_register);
    SCENARIO("a disconnected bus fails the read", a_disconnected_bus_fails_the_read);
    SCENARIO("a scaled parameter is read in display units",
             a_scaled_parameter_is_read_in_display_units);
    SCENARIO("a scaled demo read reports display units",
             a_scaled_demo_read_reports_display_units);
    SCENARIO("a scaled write encodes back to wire units",
             a_scaled_write_encodes_back_to_wire_units);
    SCENARIO("an option write sends the wire code not the index",
             an_option_write_sends_the_wire_code_not_the_index);
    SCENARIO("a failed bus read is reported", a_failed_bus_read_is_reported);
    SCENARIO("a successful read uses the verified register",
             a_successful_read_uses_the_verified_register);
    SCENARIO("a signed value is sign extended", a_signed_value_is_sign_extended);
    SCENARIO("demo mode reports the documented default",
             demo_mode_reports_the_documented_default);

    SCENARIO("an unknown parameter cannot be written",
             an_unknown_parameter_cannot_be_written);
    SCENARIO("an out of range value is refused", an_out_of_range_value_is_refused);
    SCENARIO("an out of range value is refused in demo mode too",
             an_out_of_range_value_is_refused_in_demo_mode_too);
    SCENARIO("a valid write reaches the wire", a_valid_write_reaches_the_wire);
    SCENARIO("a disconnected write reports ok but not sent",
             a_disconnected_write_reports_ok_but_not_sent);
    SCENARIO("a rejected bus write is reported", a_rejected_bus_write_is_reported);
    SCENARIO("demo mode accepts a valid write without touching the bus",
             demo_mode_accepts_a_valid_write_without_touching_the_bus);
    SCENARIO("a null bus_ok is tolerated", a_null_bus_ok_is_tolerated);

    SCENARIO("an unknown option is refused", an_unknown_option_is_refused);
    SCENARIO("an option write puts the wire code on the bus",
             an_option_write_puts_the_wire_code_on_the_bus);
    SCENARIO("a disconnected option write reports ok but not sent",
             a_disconnected_option_write_reports_ok_but_not_sent);
    SCENARIO("a rejected option bus write is reported",
             a_rejected_option_bus_write_is_reported);
    SCENARIO("demo mode accepts a valid option without touching the bus",
             demo_mode_accepts_a_valid_option_without_touching_the_bus);
    SCENARIO("an invalid option is refused in demo mode too",
             an_invalid_option_is_refused_in_demo_mode_too);
    SCENARIO("a null bus_ok is tolerated for options",
             a_null_bus_ok_is_tolerated_for_options);

    SCENARIO("a read only parameter can be read but not written",
             a_read_only_parameter_can_be_read_but_not_written);
    SCENARIO("a locked parameter can be read but not written",
             a_locked_parameter_can_be_read_but_not_written);

#undef SCENARIO

    if (failed == 0) {
        std::printf("advanced_params: %d scenarios passed\n", total);
        std::fprintf(stderr, "advanced_params: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "advanced_params: %d of %d scenarios FAILED\n", failed,
                 total);
    return 1;
}
