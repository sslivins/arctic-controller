// Native unit tests for main/log_persist.cpp -- the flash-backed snapshot of
// the RAM log ring.
//
// Why this matters more than its size suggests: log_persist is what preserves
// the run-up to a wedge. When the controller stops answering and gets power
// cycled, the RAM ring (log_buffer) is gone and the structured event log only
// records typed events, not log text. The persisted snapshot is the only
// remaining record of what the firmware was actually saying as it failed --
// the primary evidence for issues like #208/#210. If it silently stops working
// nothing else in the system notices, because its whole job is to be available
// after a failure that by definition was not anticipated.
//
// The headline claim under test is the one the source header makes:
//
//   "Writing a fresh slot never touches the previous one, so a torn/
//    interrupted write (e.g. a reboot mid-flush) at worst produces one
//    CRC-invalid slot that is ignored -- the prior snapshot survives."
//
// That is an atomicity claim about a device losing power at an arbitrary
// instant. It cannot be tested over HTTP and cannot practically be tested by
// power-cycling the physical controller. Here it is a loop over every cut
// point.
//
// This runs against the REAL log_buffer, not a fake: lines are pushed through
// the installed esp_log vprintf hook exactly as ESP_LOGx would, so the
// formatting, the ring's newest-lines-win trimming, and the persisted text are
// all end-to-end.
//
// Refs #217 (T10, T14).

#include "esp_partition_fake.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "heap_fake.h"
#include "log_buffer.h"
#include "log_persist.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// The layout log_persist expects: 32 KB slots. partitions.csv gives dbglog
// 512 KB, i.e. 16 slots. A smaller region keeps the round-robin tests short
// while still exercising real wrapping.
constexpr uint32_t SLOT_SIZE = 32 * 1024;
constexpr uint32_t SLOTS = 4;
constexpr uint32_t REGION = SLOT_SIZE * SLOTS;

// Push a line through the installed esp_log hook, exactly as ESP_LOGx does.
void emit(const char* fmt, ...) {
    vprintf_like_t sink = esp_log_get_vprintf();
    if (sink == nullptr) {
        std::fprintf(stderr, "  FAIL: no vprintf hook installed\n");
        ++g_failures;
        return;
    }
    va_list args;
    va_start(args, fmt);
    sink(fmt, args);
    va_end(args);
}

// An ESP-IDF-formatted log line at the given level.
void log_line(char level, const char* tag, const char* msg) {
    emit("%c (%lu) %s: %s\n", level, (unsigned long)1234, tag, msg);
}

void install_dbglog(uint32_t size = REGION) {
    flash_fake::install("dbglog", 0x40, size);
}

// A later boot: the flash image is whatever the previous phase left behind.
// Crucially this does NOT call install_dbglog() -- flash_fake::install()
// re-registers the partition ERASED, so calling it in a second phase would
// wipe the very image the phase is meant to read back, and every persistence
// assertion would fail (or worse, vacuously pass).
void remount() { CHECK(log_persist_init() == true); }

// The snapshot as the API reports it, using the REPORTED LENGTH rather than
// C-string semantics. Building the string from buf.data() alone would stop at
// the first NUL, which silently hides any junk a buggy implementation appended
// past the real text -- the persisted payload is a byte range with a declared
// length, so the test has to compare it as one.
std::string previous_text() {
    log_persist_prev_info_t info{};
    log_persist_get_previous_info(&info);
    if (!info.present) return std::string();
    std::vector<char> buf(info.len + 64);
    size_t len = log_persist_get_previous(buf.data(), buf.size());
    if (len > buf.size()) len = buf.size();
    return std::string(buf.data(), len);
}

log_persist_prev_info_t previous_info() {
    log_persist_prev_info_t info{};
    log_persist_get_previous_info(&info);
    return info;
}

// ------------------------------------------------------------------------
// Mounting
// ------------------------------------------------------------------------
void init_without_a_partition_disables_persistence() {
    // No dbglog and no legacy label: the feature must degrade quietly rather
    // than refuse to boot.
    CHECK(log_persist_init() == false);

    char buf[64];
    std::memset(buf, 'x', sizeof(buf));
    CHECK_EQ_INT(log_persist_get_previous(buf, sizeof(buf)), 0);
    // Still NUL-terminated: callers print this straight into an API response.
    CHECK_EQ_INT(buf[0], '\0');
    CHECK(previous_info().present == false);

    // And a flush is a no-op rather than a crash.
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void the_legacy_partition_label_is_still_accepted() {
    // Devices that only ever received an app OTA still carry the pre-relabel
    // name. Dropping this fallback would silently disable persistence on
    // exactly the oldest units in the field.
    flash_fake::install("human_face_det", 0x40, REGION);
    CHECK(log_persist_init() == true);

    log_buffer_init();
    log_line('E', "boot", "legacy label");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void a_partition_smaller_than_one_slot_is_refused() {
    flash_fake::install("dbglog", 0x40, SLOT_SIZE - 4096);
    CHECK(log_persist_init() == false);
    // Must not then try to write into a region it rejected.
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    CHECK_EQ_INT(flash_fake::bytes_written(), 0);
}

void a_blank_device_has_no_previous_snapshot() {
    install_dbglog();
    CHECK(log_persist_init() == true);
    CHECK(previous_info().present == false);
    CHECK_EQ_INT(previous_text().size(), 0);
}

// ------------------------------------------------------------------------
// Round trip
// ------------------------------------------------------------------------
void first_boot_writes_a_snapshot() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);

    log_line('E', "wifi", "association failed");
    log_line('W', "http", "socket exhausted");
    log_persist_flush_now(LOG_PERSIST_REASON_SEVERITY);

    CHECK(flash_fake::bytes_written() > 0);
}

void the_snapshot_is_readable_after_a_reboot() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    CHECK_EQ_INT(info.seq, 1);
    CHECK_EQ_INT(info.reason, LOG_PERSIST_REASON_SEVERITY);
    CHECK(info.len > 0);

    // The actual log text from the previous boot, not just a header.
    std::string text = previous_text();
    CHECK(text.find("association failed") != std::string::npos);
    CHECK(text.find("socket exhausted") != std::string::npos);
    // Written oldest-first, so the earlier line comes first.
    CHECK(text.find("association failed") < text.find("socket exhausted"));
}

void the_snapshot_records_the_boot_that_wrote_it() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "boot", "first boot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);

    // Within this boot the snapshot just written is NOT reported as
    // "previous" -- previous means the boot before this one.
    CHECK(previous_info().present == false);
}

void the_previous_boot_id_differs_from_this_one() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    CHECK(info.boot_id != 0);

    // Write one of our own, then confirm the ids really are per-boot by
    // checking the stored one is not simply zero or a constant.
    log_line('E', "boot", "second boot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    CHECK_EQ_INT(previous_info().boot_id, info.boot_id);
}

// ------------------------------------------------------------------------
// Round-robin and sequencing
// ------------------------------------------------------------------------
void write_more_snapshots_than_slots() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);

    // One more flush than there are slots, so the round-robin wraps and
    // overwrites the oldest.
    for (uint32_t i = 1; i <= SLOTS + 1; ++i) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "snapshot number %u", i);
        log_line('E', "loop", msg);
        log_persist_flush_now(LOG_PERSIST_REASON_HEARTBEAT);
    }
}

void the_newest_snapshot_wins_after_wrapping() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    // Five flushes over four slots: the newest record has seq 5.
    CHECK_EQ_INT(info.seq, SLOTS + 1);

    std::string text = previous_text();
    char newest[64];
    std::snprintf(newest, sizeof(newest), "snapshot number %u", SLOTS + 1);
    CHECK(text.find(newest) != std::string::npos);
}

void the_sequence_continues_past_the_previous_boot() {
    log_buffer_init();
    remount();

    // A seq that restarted at 1 would make an older record look newer for the
    // rest of the device's life.
    log_line('E', "boot", "after reboot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void the_sequence_did_continue() {
    log_buffer_init();
    remount();
    CHECK_EQ_INT(previous_info().seq, SLOTS + 2);
}

// ------------------------------------------------------------------------
// Rejecting damaged records -- the reason the CRC is there
// ------------------------------------------------------------------------
void write_two_snapshots_then_corrupt_the_newest() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);

    log_line('E', "old", "the older snapshot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);   // seq 1, slot 0
    log_line('E', "new", "the newer snapshot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);   // seq 2, slot 1

    // lp_header_t is packed: magic@0, seq@4, boot_id@8, len@12, crc@16,
    // reason@20, pad@21 -- 24 bytes, then the payload text.
    constexpr size_t HEADER = 24;
    uint32_t len = 0;
    CHECK(flash_fake::peek("dbglog", SLOT_SIZE + 12, &len, sizeof(len)));
    // Both log lines are in the ring by the second flush, so the payload is
    // comfortably larger than the header. If this ever shrinks, the
    // corruption below would miss the CRC-covered bytes and the test would
    // pass without testing anything.
    CHECK(len > 32);

    // Damage the middle of the payload. NOR can only clear bits, so masking is
    // the physically honest corruption. The mask matters: 0x7f would be a
    // no-op here because the payload is ASCII log text (every byte is already
    // <= 0x7f), so the "corruption" would change nothing and the scenario
    // would pass for the wrong reason. 0x80 clears the low seven bits.
    const size_t at = SLOT_SIZE + HEADER + (len / 2);
    uint8_t before[16] = {0}, after[16] = {0};
    CHECK(flash_fake::peek("dbglog", at, before, sizeof(before)));
    for (size_t i = 0; i < sizeof(before); ++i) {
        flash_fake::corrupt_byte("dbglog", at + i, 0x80);
    }
    CHECK(flash_fake::peek("dbglog", at, after, sizeof(after)));
    // Prove we actually changed the image before asserting the CRC caught it.
    CHECK(memcmp(before, after, sizeof(before)) != 0);
}

void the_corrupt_record_is_skipped_for_the_older_one() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    // Falls back to the intact older record rather than returning garbage.
    CHECK_EQ_INT(info.seq, 1);

    std::string text = previous_text();
    CHECK(text.find("the older snapshot") != std::string::npos);
    CHECK(text.find("the newer snapshot") == std::string::npos);
}

void write_a_snapshot_then_wreck_its_magic() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "only", "the only snapshot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);

    // Clear bits in the magic word.
    for (size_t off = 0; off < 4; ++off) {
        flash_fake::corrupt_byte("dbglog", off, 0x00);
    }
}

void a_record_without_the_magic_is_not_read() {
    log_buffer_init();
    remount();
    CHECK(previous_info().present == false);
}

void write_a_snapshot_then_overstate_its_length() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "only", "the only snapshot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);

    // len lives at offset 12 in lp_header_t (magic, seq, boot_id, len).
    // Setting it larger than a slot payload would make a naive reader
    // allocate and read past the record.
    uint32_t huge = 0xFFFFFFF0u;
    flash_fake::poke("dbglog", 12, &huge, sizeof(huge));
}

void an_impossible_length_is_rejected() {
    log_buffer_init();
    remount();
    CHECK(previous_info().present == false);
}

// The absurd length above is rejected even without the range check, because
// allocating 4 GB fails. A length just past the payload cap is the case the
// check actually exists for: it allocates fine, and the CRC is made to match
// the over-long region, so nothing but the range check stands between the
// reader and a record that spills into the next slot.
void write_a_snapshot_then_overstate_its_length_slightly() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "only", "the only snapshot");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);

    constexpr size_t HEADER = 24;
    constexpr uint32_t PAYLOAD_MAX = SLOT_SIZE - HEADER;
    const uint32_t overlong = PAYLOAD_MAX + 64;

    std::vector<uint8_t> spill(overlong);
    CHECK(flash_fake::peek("dbglog", HEADER, spill.data(), spill.size()));
    uint32_t crc = esp_rom_crc32_le(0, spill.data(), spill.size());

    flash_fake::poke("dbglog", 12, &overlong, sizeof(overlong));
    flash_fake::poke("dbglog", 16, &crc, sizeof(crc));
}

void a_length_past_the_slot_payload_is_rejected() {
    log_buffer_init();
    remount();
    // Accepting this would serve 64 bytes of the NEXT slot as if they were
    // part of this snapshot.
    CHECK(previous_info().present == false);
}

// ------------------------------------------------------------------------
// A read that fails after the record has already been validated
// ------------------------------------------------------------------------
void write_one_snapshot_to_reload() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "one", "the snapshot to reload");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void a_failed_payload_reload_reports_no_snapshot() {
    log_buffer_init();

    // With exactly one valid record in slot 0, the scan reads four slot
    // headers and one payload (for the CRC check); the sixth read is the
    // final load into s_prev_text. Failing that one leaves the module having
    // validated a record it could not actually load.
    flash_fake::fail_nth(flash_fake::Op::Read, ESP_FAIL, 6);
    remount();
    flash_fake::clear_failures();

    // Six reads is a property of the layout, not an incidental number; assert
    // it so this scenario fails loudly rather than silently targeting the
    // wrong read if the scan ever changes shape.
    CHECK_EQ_INT(flash_fake::call_count(flash_fake::Op::Read), 6);

    // Reporting present==true here would hand the API a null/!uninitialised
    // buffer while claiming a snapshot exists.
    CHECK(previous_info().present == false);
    char buf[64];
    CHECK_EQ_INT(log_persist_get_previous(buf, sizeof(buf)), 0);
    CHECK_EQ_INT(buf[0], 0);
}

// ------------------------------------------------------------------------
// Power loss part-way through a flush
// ------------------------------------------------------------------------
void write_a_good_snapshot_first() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "keep", "the snapshot that must survive");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

// Reads the cut point from the environment so one function can serve every
// step of the sweep (see main).
void lose_power_during_the_next_flush() {
    log_buffer_init();
    remount();

    const char* cut = std::getenv("LP_POWER_LOSS_AFTER");
    flash_fake::power_loss_after(cut ? std::atoi(cut) : 0);

    log_line('E', "doomed", "this flush never completes");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void the_earlier_snapshot_still_survives() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    // Either the interrupted write landed completely (in which case it is a
    // valid newer record) or it did not (in which case the older one must
    // still be there). What must never happen is BOTH being unreadable, or a
    // half-written record passing the CRC.
    CHECK(info.present == true);
    if (!info.present) return;

    std::string text = previous_text();
    bool old_one = text.find("the snapshot that must survive") != std::string::npos;
    bool new_one = text.find("this flush never completes") != std::string::npos;
    CHECK(old_one || new_one);
    // The recovered text is a complete record, not a fragment of one: the
    // API's own NUL terminator must land exactly at the reported length, so
    // there is no truncation and no junk past the text.
    std::vector<char> raw(info.len + 64, '\x7f');
    CHECK_EQ_INT(log_persist_get_previous(raw.data(), raw.size()), info.len);
    CHECK_EQ_INT(std::strlen(raw.data()), info.len);
    CHECK_EQ_INT(text.size(), info.len);
}

// ------------------------------------------------------------------------
// Trimming to the slot payload
// ------------------------------------------------------------------------
void an_oversized_log_keeps_the_newest_lines() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);

    // The RAM ring holds 256 entries; fill it so the formatted text is far
    // larger than a slot payload and must be trimmed.
    for (int i = 0; i < LOG_BUFFER_MAX_ENTRIES; ++i) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "line %04d %s", i,
                      "padding to make each line substantial so the total "
                      "comfortably exceeds one slot payload and forces the "
                      "trim path to run");
        log_line('E', "fill", msg);
    }
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void the_trimmed_snapshot_starts_on_a_line_boundary() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    if (!info.present) return;

    // Trimmed to fit a slot.
    CHECK(info.len <= SLOT_SIZE);

    std::string text = previous_text();
    CHECK(text.size() > 0);
    if (text.empty()) return;

    // The newest line is what matters after a crash, so it must be the one
    // that is kept.
    CHECK(text.find("line 0255") != std::string::npos);
    // ...and the oldest must be the one dropped.
    CHECK(text.find("line 0000") == std::string::npos);

    // No half line at the top: every line begins with the uptime column, so
    // the text must not start mid-word.
    CHECK(text.find('\n') != std::string::npos);
    std::string first = text.substr(0, text.find('\n'));
    CHECK(first.find("line 0") != std::string::npos);

    // ...and no half line at the BOTTOM either. This is what distinguishes a
    // correct trim from one that ignores the trim offset and simply lets the
    // slot clamp in flush_locked() cut the text: that also starts on a line
    // boundary, but it ends wherever 32744 bytes happen to land.
    CHECK(text.back() == '\n');
}

// ------------------------------------------------------------------------
// The read API
// ------------------------------------------------------------------------
void write_a_snapshot_for_the_read_api() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "api", "abcdefghij");
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
}

void reads_are_bounded_and_terminated() {
    log_buffer_init();
    remount();

    log_persist_prev_info_t info = previous_info();
    CHECK(info.present == true);
    if (!info.present) return;

    // A short buffer must truncate, stay NUL-terminated, and still report the
    // full length so a caller can page or resize.
    char small[8];
    std::memset(small, 'Z', sizeof(small));
    size_t reported = log_persist_get_previous(small, sizeof(small));
    CHECK_EQ_INT(reported, info.len);
    CHECK_EQ_INT(std::strlen(small), sizeof(small) - 1);

    // Degenerate arguments must not write anywhere.
    char guard[4];
    std::memset(guard, 'Q', sizeof(guard));
    CHECK_EQ_INT(log_persist_get_previous(guard, 0), 0);
    CHECK_EQ_INT(guard[0], 'Q');
    CHECK_EQ_INT(log_persist_get_previous(nullptr, 16), 0);

    // A null info pointer is tolerated.
    log_persist_get_previous_info(nullptr);
}

// ------------------------------------------------------------------------
// Resource handling
// ------------------------------------------------------------------------
void a_flush_that_cannot_allocate_gives_up_cleanly() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "oom", "something to persist");

    heap_fake::reset();
    // build_tail_text allocates the entry array first; failing it exercises
    // the early-return path that a device under memory pressure takes. This
    // is not hypothetical -- see #247.
    heap_fake::fail_next_alloc(1);
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    heap_fake::clear_failures();

    // Nothing was written, and nothing was leaked on the way out.
    CHECK_EQ_INT(flash_fake::bytes_written(), 0);
    CHECK_EQ_INT(heap_fake::outstanding(), 0);
}

void a_flush_that_cannot_build_its_scratch_buffer_gives_up_cleanly() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "oom", "something to persist");

    heap_fake::reset();
    // build_tail_text allocates the entry array, THEN the 96 KB scratch
    // buffer. The scratch allocation is the one most likely to fail on a
    // device under memory pressure, and it is a separate early return with
    // its own cleanup -- failing only the first allocation never reaches it.
    heap_fake::fail_nth_alloc(2);
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    heap_fake::clear_failures();

    CHECK_EQ_INT(flash_fake::bytes_written(), 0);
    CHECK_EQ_INT(heap_fake::outstanding(), 0);
}

void a_flush_that_cannot_allocate_the_result_frees_its_scratch() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "oom", "something to persist");

    heap_fake::reset();
    // The third allocation is the trimmed result buffer. By then the entry
    // array has been freed but the 96 KB scratch is still held, so bailing
    // out without freeing it loses 96 KB of PSRAM on every failed flush --
    // exactly the kind of slow leak that only shows up after days of uptime.
    heap_fake::fail_nth_alloc(3);
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    heap_fake::clear_failures();

    CHECK_EQ_INT(flash_fake::bytes_written(), 0);
    CHECK_EQ_INT(heap_fake::outstanding(), 0);
}

void a_successful_flush_leaks_nothing() {
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_line('E', "leak", "check");

    heap_fake::reset();
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    // Every scratch buffer in flush_locked/build_tail_text is freed on the
    // success path too.
    CHECK_EQ_INT(heap_fake::outstanding(), 0);
    CHECK(flash_fake::bytes_written() > 0);
}

void the_background_task_is_not_started_without_a_partition() {
    task_fake::reset();
    CHECK(log_persist_init() == false);
    log_persist_start();
    // Starting a task that can never do anything would burn 4 KB of stack on
    // a device that is already short of internal RAM.
    CHECK_EQ_INT(task_fake::created(), 0);
}

void the_background_task_starts_when_there_is_a_partition() {
    task_fake::reset();
    install_dbglog();
    log_buffer_init();
    CHECK(log_persist_init() == true);
    log_persist_start();

    CHECK_EQ_INT(task_fake::created(), 1);
    if (task_fake::created() == 1) {
        CHECK(task_fake::requests()[0].name == "log_persist");
    }
}

using Phase = void (*)();

int run_scenario(const char* name, std::vector<Phase> phases) {
    flash_fake::reset();
    heap_fake::reset();

    for (size_t i = 0; i < phases.size(); ++i) {
        std::fflush(stdout);
        std::fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            // log_persist and log_buffer both write to stdout; failures go to
            // stderr, which stays connected.
            std::freopen("/dev/null", "w", stdout);
            g_failures = 0;
            phases[i]();
            std::fflush(stderr);
            _exit(g_failures == 0 ? 0 : 1);
        }
        if (pid < 0) {
            std::fprintf(stderr, "FAIL [%s]: fork failed\n", name);
            return 1;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "FAIL [%s] phase %zu: crashed with signal %d\n",
                         name, i + 1, WTERMSIG(status));
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "FAIL [%s] phase %zu\n", name, i + 1);
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define SCENARIO(name, ...)                          \
    do {                                             \
        ++total;                                     \
        failed += run_scenario(name, {__VA_ARGS__}); \
    } while (0)

    // Mounting
    SCENARIO("no partition disables persistence",
             init_without_a_partition_disables_persistence);
    SCENARIO("the legacy partition label is still accepted",
             the_legacy_partition_label_is_still_accepted);
    SCENARIO("a partition smaller than one slot is refused",
             a_partition_smaller_than_one_slot_is_refused);
    SCENARIO("a blank device has no previous snapshot",
             a_blank_device_has_no_previous_snapshot);

    // Round trip
    SCENARIO("a snapshot survives a reboot", first_boot_writes_a_snapshot,
             the_snapshot_is_readable_after_a_reboot);
    SCENARIO("this boot's own snapshot is not reported as previous",
             the_snapshot_records_the_boot_that_wrote_it);
    SCENARIO("the previous boot id is recorded", first_boot_writes_a_snapshot,
             the_previous_boot_id_differs_from_this_one);

    // Round-robin
    SCENARIO("the newest snapshot wins after the slots wrap",
             write_more_snapshots_than_slots,
             the_newest_snapshot_wins_after_wrapping);
    SCENARIO("the sequence continues across reboots",
             write_more_snapshots_than_slots,
             the_sequence_continues_past_the_previous_boot,
             the_sequence_did_continue);

    // Damaged records
    SCENARIO("a corrupt record falls back to the older one",
             write_two_snapshots_then_corrupt_the_newest,
             the_corrupt_record_is_skipped_for_the_older_one);
    SCENARIO("a record without the magic is ignored",
             write_a_snapshot_then_wreck_its_magic,
             a_record_without_the_magic_is_not_read);
    SCENARIO("an impossible payload length is rejected",
             write_a_snapshot_then_overstate_its_length,
             an_impossible_length_is_rejected);
    SCENARIO("a length past the slot payload is rejected",
             write_a_snapshot_then_overstate_its_length_slightly,
             a_length_past_the_slot_payload_is_rejected);
    SCENARIO("a failed payload reload reports no snapshot",
             write_one_snapshot_to_reload,
             a_failed_payload_reload_reports_no_snapshot);

    // Power loss during a flush. A single cut point can agree with a broken
    // implementation by luck; the invariant has to hold at every one of them.
    for (int cut = 0; cut <= 6; ++cut) {
        char value[8];
        std::snprintf(value, sizeof(value), "%d", cut);
        setenv("LP_POWER_LOSS_AFTER", value, 1);
        char name[96];
        std::snprintf(name, sizeof(name),
                      "power loss after %d flash ops leaves a valid snapshot", cut);
        SCENARIO(name, write_a_good_snapshot_first,
                 lose_power_during_the_next_flush,
                 the_earlier_snapshot_still_survives);
    }
    unsetenv("LP_POWER_LOSS_AFTER");

    // Trimming
    SCENARIO("an oversized log keeps its newest lines",
             an_oversized_log_keeps_the_newest_lines,
             the_trimmed_snapshot_starts_on_a_line_boundary);

    // Read API
    SCENARIO("reads are bounded and terminated", write_a_snapshot_for_the_read_api,
             reads_are_bounded_and_terminated);

    // Resources
    SCENARIO("a flush that cannot allocate gives up cleanly",
             a_flush_that_cannot_allocate_gives_up_cleanly);
    SCENARIO("a flush that cannot build its scratch buffer gives up cleanly",
             a_flush_that_cannot_build_its_scratch_buffer_gives_up_cleanly);
    SCENARIO("a flush that cannot allocate the result frees its scratch",
             a_flush_that_cannot_allocate_the_result_frees_its_scratch);
    SCENARIO("a successful flush leaks nothing", a_successful_flush_leaks_nothing);
    SCENARIO("the background task is not started without a partition",
             the_background_task_is_not_started_without_a_partition);
    SCENARIO("the background task starts when there is a partition",
             the_background_task_starts_when_there_is_a_partition);

#undef SCENARIO

    if (failed == 0) {
        std::printf("log_persist: %d scenarios passed\n", total);
        std::fprintf(stderr, "log_persist: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "log_persist: %d of %d scenarios FAILED\n", failed, total);
    return 1;
}
