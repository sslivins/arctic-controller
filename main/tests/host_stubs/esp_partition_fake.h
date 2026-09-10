/*
 * Test-only controls for the RAM-backed flash fake.
 *
 * Kept out of esp_partition.h so production sources cannot reach them.
 *
 * The power-loss control is the reason this fake exists. history_storage.cpp
 * claims that "the old committed bank remains valid until the replacement bank
 * is fully written and committed" -- an atomicity claim that can only be tested
 * by cutting power in the middle of a compaction. On the physical controller
 * that means pulling the plug at exactly the right microsecond, thousands of
 * times, and reflashing after each attempt. Here it is one function call.
 *
 * Partition images live in shared anonymous memory, so they survive a fork.
 * That is deliberate: history_storage.cpp's init() early-returns once it has a
 * partition, so an in-process "reboot" is a no-op and any persistence
 * assertion written that way passes without ever reading flash. Tests get a
 * real power cycle by running each phase in a forked child -- cold statics,
 * same medium.
 */
#pragma once

#include "esp_partition.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flash_fake {

// Install a partition table entry backed by `size` bytes of RAM, erased
// (0xff). Replaces any partition already registered under `label`.
void install(const std::string &label, uint8_t subtype, uint32_t size);

// The production layout: a 2 MB "history" partition, subtype 0x40, matching
// partitions.csv. Use this unless a test is specifically about sizing.
void install_history_partition();

// Drop all partitions and clear every counter and injected fault.
void reset();

// --- fault and power-loss injection ------------------------------------
// After `n` more successful flash operations (writes + erases), every
// subsequent write and erase silently does nothing while still returning
// ESP_OK. This models the device losing power mid-sequence: the code believes
// it completed, but the bytes never reached the medium.
void power_loss_after(int n);

// Make the next `count` calls of the given operation return `err`, without
// touching the medium. `count < 0` means until reset or cleared.
enum class Op { Read, Write, Erase };
void fail_next(Op op, esp_err_t err, int count = 1);

// Fail only the `nth` (1-based) subsequent call of the given operation,
// letting the ones before it succeed. Needed when the interesting failure is
// not the first of its kind -- e.g. a read error on the final payload load
// after the scan has already read every slot header successfully.
void fail_nth(Op op, esp_err_t err, int nth);
void clear_failures();

// --- inspection ---------------------------------------------------------
int call_count(Op op);
// Bytes actually erased / written since reset. Lets a test show a code path
// really did rewrite a bank rather than passing because it did nothing.
size_t bytes_written();
size_t bytes_erased();

// Raw access to the backing image, bypassing all injection.
bool peek(const std::string &label, size_t offset, void *dst, size_t size);
void poke(const std::string &label, size_t offset, const void *src, size_t size);

// Flip bits in the image to model bit rot / a corrupt sector. Unlike poke this
// is honest about NOR: it can only clear bits, never set them.
void corrupt_byte(const std::string &label, size_t offset, uint8_t and_mask);

// True when every byte in the range is 0xff.
bool is_erased(const std::string &label, size_t offset, size_t size);

}  // namespace flash_fake
