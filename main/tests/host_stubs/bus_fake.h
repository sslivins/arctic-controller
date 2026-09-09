// Fake Macon bus for host tests.
//
// Only the three entry points main/advanced_params.cpp uses to reach the heat
// pump are faked, and they are faked because they are hardware: an RS485
// transaction with a mainboard. Everything else in the advanced-parameter path
// -- the parameter table, the range/enum guardrail, the wire encoding -- is the
// real arctic-macon code, linked as-is. Faking any of that would mean testing a
// stand-in for the logic under test.
#pragma once

#include <stdint.h>

#include <vector>

namespace bus_fake {

struct Write {
    uint16_t reg;
    uint16_t raw;
};

// Link state reported to the module under test.
void set_connected(bool connected);

// The value the next successful readRegister returns, and whether reads
// succeed at all.
void set_read_value(uint16_t raw);
void set_read_ok(bool ok);

// Whether writes report success.
void set_write_ok(bool ok);

// Everything the module put on the wire this scenario, in order.
const std::vector<Write>& writes();
const std::vector<uint16_t>& reads();

void reset();

}  // namespace bus_fake
