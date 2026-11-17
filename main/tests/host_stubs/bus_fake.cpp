#include "bus_fake.h"

#include "heatpump_controller.h"
#include "macon_advanced_params.h"

namespace {
bool g_connected = true;
bool g_read_ok = true;
bool g_write_ok = true;
uint16_t g_read_value = 0;
std::vector<bus_fake::Write> g_writes;
std::vector<uint8_t> g_reads;
}  // namespace

namespace bus_fake {

void set_connected(bool connected) { g_connected = connected; }
void set_read_value(uint16_t raw) { g_read_value = raw; }
void set_read_ok(bool ok) { g_read_ok = ok; }
void set_write_ok(bool ok) { g_write_ok = ok; }

const std::vector<Write>& writes() { return g_writes; }
const std::vector<uint8_t>& reads() { return g_reads; }

void reset() {
    g_connected = true;
    g_read_ok = true;
    g_write_ok = true;
    g_read_value = 0;
    g_writes.clear();
    g_reads.clear();
}

}  // namespace bus_fake

namespace arctic {

bool isConnected() { return g_connected; }

bool readAdvancedParam(uint8_t ap, uint16_t* raw_out) {
    g_reads.push_back(ap);
    if (!g_read_ok) return false;
    if (raw_out) *raw_out = g_read_value;
    return true;
}

bool writeAdvancedParam(const AdvWritePlan& plan) {
    // An invalid plan never reaches the wire on the real path either.
    if (!plan.valid()) return false;
    // Recorded even when the write is reported as failing: "the module tried to
    // put this on the wire" and "the wire accepted it" are different facts, and
    // a scenario that asserts nothing was sent needs the first one.
    g_writes.push_back({plan.ap(), plan.raw()});
    return g_write_ok;
}

}  // namespace arctic