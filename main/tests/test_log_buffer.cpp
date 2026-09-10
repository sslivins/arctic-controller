// Native unit tests for main/log_buffer.cpp -- the RAM ring buffer behind
// GET /api/logs and behind the crash/warning polling the device tests rely on.
//
// What existed before: tests/api/test_logs_api.py, which asks the device for
// whatever happens to be in the buffer and checks the response shape. It
// cannot fill 256 entries to force a wrap, cannot control the sequence
// numbers, and cannot feed the parser a malformed line -- so the ring
// arithmetic, the since_seq/min_level filters, and the ESP-IDF log-format
// parser were all untested.
//
// The claims under test are the ones the header makes:
//   * "Returns entries in chronological order (oldest first)" -- including
//     after the ring has wrapped, which is where an off-by-one lives
//   * since_seq returns strictly newer entries, so a client polling
//     incrementally never sees a duplicate and never skips one
//   * "Don't reset s_next_seq -- clients tracking by seq need monotonic
//     growth" (log_buffer_clear)
//   * log_buffer_latest_seq_at_level agrees with the filter in
//     log_buffer_get, since a disagreement means "has a new error appeared?"
//     answers differently from "show me the errors"
//
// ISOLATION: log_buffer.cpp keeps the ring, the head and the sequence counter
// in file-scope statics, and init() installs a global hook. Each test runs in
// its own forked child so those start cold.

#include "esp_timer.h"
#include "log_buffer.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
static int g_failures = 0;

// Failures go to stderr: the hook under test writes captured log lines to
// stdout (that is the serial passthrough), and each child redirects stdout to
// /dev/null so 256-entry wrap tests do not bury the results.
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

namespace {

// Drive the installed hook exactly as esp_log would: a format string and
// varargs. Going through the hook rather than a test-only entry point means
// the parser is under test too.
void emit(const char *fmt, ...) {
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

void log_line(char level, const char *tag, const char *message) {
    emit("%c (%u) %s: %s\n", level, 1234u, tag, message);
}

std::vector<log_entry_t> fetch(int max_count = LOG_BUFFER_MAX_ENTRIES,
                               uint32_t since_seq = 0,
                               esp_log_level_t min_level = ESP_LOG_VERBOSE) {
    std::vector<log_entry_t> out(max_count);
    const int n = log_buffer_get(out.data(), max_count, since_seq, min_level);
    out.resize(n < 0 ? 0 : n);
    return out;
}

void start() {
    timer_fake::reset();
    log_buffer_init();
}

// ---------------------------------------------------------------------------
// Parsing. The hook receives an already-formatted ESP-IDF log line; anything
// it mis-parses is silently wrong in the API response forever after.
// ---------------------------------------------------------------------------

void a_plain_log_line_is_split_into_level_tag_and_message() {
    start();
    log_line('I', "wifi", "connected to Arctic");

    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (entries.empty()) return;
    CHECK(entries[0].level == ESP_LOG_INFO);
    CHECK(std::string(entries[0].tag) == "wifi");
    CHECK(std::string(entries[0].message) == "connected to Arctic");
    CHECK(entries[0].seq == 1);
}

void every_level_character_maps_to_its_level() {
    start();
    log_line('E', "t", "e");
    log_line('W', "t", "w");
    log_line('I', "t", "i");
    log_line('D', "t", "d");
    log_line('V', "t", "v");

    auto entries = fetch();
    CHECK(entries.size() == 5);
    if (entries.size() != 5) return;
    CHECK(entries[0].level == ESP_LOG_ERROR);
    CHECK(entries[1].level == ESP_LOG_WARN);
    CHECK(entries[2].level == ESP_LOG_INFO);
    CHECK(entries[3].level == ESP_LOG_DEBUG);
    CHECK(entries[4].level == ESP_LOG_VERBOSE);
}

// Colour is on by default in ESP-IDF builds, so most real lines arrive wrapped
// in escape sequences. A parser that kept them would put "\033[0;32m" in the
// level slot and drop the line.
void colour_escapes_are_stripped_from_both_ends() {
    start();
    emit("\033[0;31m%c (%u) %s: %s\033[0m\n", 'E', 99u, "boot", "brownout");

    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (entries.empty()) return;
    CHECK(entries[0].level == ESP_LOG_ERROR);
    CHECK(std::string(entries[0].tag) == "boot");
    CHECK(std::string(entries[0].message) == "brownout");
}

// Raw printf output (no level/tag structure) must be forwarded to serial but
// not stored, or the buffer fills with bootloader noise. Each of these fails
// at a different point in the parser, and each mutation-tested to a distinct
// mutant -- a line can look like a log entry right up until the last field.
void a_line_that_is_not_a_log_entry_is_not_stored() {
    start();
    emit("just some raw output\n");                 // no level char
    emit("Z (123) tag: unknown level char\n");      // unknown level char
    emit("I no parenthesis here\n");                // no timestamp, no separator
    emit("I looks like a tag: but has no timestamp\n");  // separator, no timestamp
    emit("I (123) timestamp but no separator\n");   // timestamp, no separator
    CHECK(log_buffer_count() == 0);
}

void an_over_long_message_is_truncated_not_overflowed() {
    start();
    const std::string huge(LOG_BUFFER_MSG_SIZE * 2, 'x');
    log_line('W', "big", huge.c_str());

    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (entries.empty()) return;
    CHECK(std::strlen(entries[0].message) == LOG_BUFFER_MSG_SIZE - 1);
    CHECK(entries[0].message[LOG_BUFFER_MSG_SIZE - 1] == '\0');
}

void an_over_long_tag_is_truncated_not_overflowed() {
    start();
    const std::string huge_tag(LOG_BUFFER_TAG_SIZE * 3, 'T');
    log_line('I', huge_tag.c_str(), "hello");

    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (entries.empty()) return;
    CHECK(std::strlen(entries[0].tag) == LOG_BUFFER_TAG_SIZE - 1);
    CHECK(entries[0].tag[LOG_BUFFER_TAG_SIZE - 1] == '\0');
}

// A message containing ": " must not be re-split: the tag ends at the FIRST
// separator, and everything after it is the message.
void a_message_containing_the_separator_survives_intact() {
    start();
    log_line('I', "http", "GET /api/logs: 200 OK");

    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (entries.empty()) return;
    CHECK(std::string(entries[0].tag) == "http");
    CHECK(std::string(entries[0].message) == "GET /api/logs: 200 OK");
}

void the_uptime_stamp_comes_from_the_clock() {
    start();
    timer_fake::set_us(5 * 1000 * 1000);
    log_line('I', "t", "first");
    timer_fake::advance_ms(1500);
    log_line('I', "t", "second");

    auto entries = fetch();
    CHECK(entries.size() == 2);
    if (entries.size() != 2) return;
    CHECK(entries[0].uptime_ms == 5000);
    CHECK(entries[1].uptime_ms == 6500);
}

// ---------------------------------------------------------------------------
// Ring arithmetic. The device suite can never fill 256 entries on demand.
// ---------------------------------------------------------------------------

void entries_come_back_oldest_first() {
    start();
    for (int i = 0; i < 10; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }
    auto entries = fetch();
    CHECK(entries.size() == 10);
    for (size_t i = 0; i < entries.size(); ++i) {
        CHECK(std::string(entries[i].message) == std::to_string(i));
        CHECK(entries[i].seq == (uint32_t)i + 1);
    }
}

// The case with the off-by-one in it: once the ring wraps, the oldest entry is
// at s_head, not at 0. Getting this wrong reorders the log without any other
// symptom.
void a_wrapped_ring_still_reads_oldest_first() {
    start();
    const int extra = 10;
    for (int i = 0; i < LOG_BUFFER_MAX_ENTRIES + extra; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }

    CHECK(log_buffer_count() == LOG_BUFFER_MAX_ENTRIES);
    auto entries = fetch();
    CHECK((int)entries.size() == LOG_BUFFER_MAX_ENTRIES);
    if ((int)entries.size() != LOG_BUFFER_MAX_ENTRIES) return;

    // The first `extra` lines were overwritten.
    CHECK(std::string(entries[0].message) == std::to_string(extra));
    CHECK(std::string(entries.back().message) ==
          std::to_string(LOG_BUFFER_MAX_ENTRIES + extra - 1));

    // Strictly increasing sequence numbers, no gaps, no repeats.
    for (size_t i = 1; i < entries.size(); ++i) {
        CHECK(entries[i].seq == entries[i - 1].seq + 1);
    }
}

void exactly_filling_the_ring_does_not_wrap() {
    start();
    for (int i = 0; i < LOG_BUFFER_MAX_ENTRIES; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }
    auto entries = fetch();
    CHECK((int)entries.size() == LOG_BUFFER_MAX_ENTRIES);
    if (entries.empty()) return;
    CHECK(std::string(entries[0].message) == "0");
    CHECK(std::string(entries.back().message) ==
          std::to_string(LOG_BUFFER_MAX_ENTRIES - 1));
}

void max_count_returns_the_oldest_entries_first() {
    start();
    for (int i = 0; i < 20; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }
    auto entries = fetch(5);
    CHECK(entries.size() == 5);
    if (entries.size() != 5) return;
    CHECK(std::string(entries[0].message) == "0");
    CHECK(std::string(entries[4].message) == "4");
}

// ---------------------------------------------------------------------------
// Filters. A client polls with since_seq; getting this wrong duplicates or
// drops log lines in the UI.
// ---------------------------------------------------------------------------

void since_seq_returns_only_strictly_newer_entries() {
    start();
    for (int i = 0; i < 10; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }

    auto entries = fetch(LOG_BUFFER_MAX_ENTRIES, 4);
    CHECK(entries.size() == 6);
    if (entries.empty()) return;
    CHECK(entries[0].seq == 5);

    // Polling again from the newest seq yields nothing, then exactly the new
    // line -- the property that makes incremental fetching correct.
    const uint32_t latest = log_buffer_get_latest_seq();
    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, latest).empty());
    log_line('W', "t", "new one");
    auto after = fetch(LOG_BUFFER_MAX_ENTRIES, latest);
    CHECK(after.size() == 1);
    if (!after.empty()) {
        CHECK(std::string(after[0].message) == "new one");
    }
}

void min_level_keeps_entries_at_least_as_severe() {
    start();
    log_line('E', "t", "error");
    log_line('W', "t", "warn");
    log_line('I', "t", "info");
    log_line('D', "t", "debug");

    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, 0, ESP_LOG_ERROR).size() == 1);
    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, 0, ESP_LOG_WARN).size() == 2);
    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, 0, ESP_LOG_INFO).size() == 3);
    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, 0, ESP_LOG_VERBOSE).size() == 4);
}

void the_two_filters_apply_together() {
    start();
    log_line('E', "t", "first error");
    log_line('I', "t", "chatter");
    const uint32_t mark = log_buffer_get_latest_seq();
    log_line('I', "t", "more chatter");
    log_line('E', "t", "second error");

    auto entries = fetch(LOG_BUFFER_MAX_ENTRIES, mark, ESP_LOG_ERROR);
    CHECK(entries.size() == 1);
    if (!entries.empty()) {
        CHECK(std::string(entries[0].message) == "second error");
    }
}

// latest_seq_at_level is the cheap poll behind "did a new error appear?". If
// it disagreed with the get() filter, the UI would announce an error it then
// could not show.
void latest_seq_at_level_agrees_with_the_get_filter() {
    start();
    log_line('I', "t", "info");
    log_line('E', "t", "error");
    log_line('I', "t", "more info");
    log_line('W', "t", "warn");
    log_line('I', "t", "yet more");

    for (esp_log_level_t level :
         {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_VERBOSE}) {
        auto entries = fetch(LOG_BUFFER_MAX_ENTRIES, 0, level);
        const uint32_t expected = entries.empty() ? 0 : entries.back().seq;
        CHECK(log_buffer_latest_seq_at_level(level) == expected);
    }
}

void latest_seq_at_level_is_zero_when_nothing_matches() {
    start();
    log_line('I', "t", "info only");
    CHECK(log_buffer_latest_seq_at_level(ESP_LOG_ERROR) == 0);
    CHECK(log_buffer_latest_seq_at_level(ESP_LOG_WARN) == 0);
    CHECK(log_buffer_latest_seq_at_level(ESP_LOG_INFO) == 1);
}

// After a wrap, the scan walks backwards from s_head across the seam.
void latest_seq_at_level_survives_a_wrap() {
    start();
    log_line('E', "t", "old error");
    for (int i = 0; i < LOG_BUFFER_MAX_ENTRIES; ++i) {
        log_line('I', "t", "filler");
    }
    // The only error has been overwritten.
    CHECK(log_buffer_latest_seq_at_level(ESP_LOG_ERROR) == 0);

    log_line('E', "t", "new error");
    CHECK(log_buffer_latest_seq_at_level(ESP_LOG_ERROR) ==
          log_buffer_get_latest_seq());
}

// ---------------------------------------------------------------------------
// Clear.
// ---------------------------------------------------------------------------

// The comment in log_buffer_clear says sequence numbers must keep growing, and
// it is right: a client that cleared the view and then polled with its old
// since_seq would otherwise be handed entries it had already seen.
void clearing_empties_the_ring_but_not_the_sequence() {
    start();
    for (int i = 0; i < 5; ++i) {
        log_line('I', "t", "x");
    }
    const uint32_t before = log_buffer_get_latest_seq();
    CHECK(before == 5);

    log_buffer_clear();
    CHECK(log_buffer_count() == 0);
    CHECK(fetch().empty());
    CHECK(log_buffer_get_latest_seq() == before);

    log_line('I', "t", "after clear");
    auto entries = fetch();
    CHECK(entries.size() == 1);
    if (!entries.empty()) {
        CHECK(entries[0].seq == before + 1);
    }
    // And an old cursor still yields only genuinely newer entries.
    CHECK(fetch(LOG_BUFFER_MAX_ENTRIES, before).size() == 1);
}

void a_cleared_ring_starts_writing_from_the_beginning() {
    start();
    for (int i = 0; i < LOG_BUFFER_MAX_ENTRIES + 5; ++i) {
        log_line('I', "t", "x");
    }
    log_buffer_clear();
    for (int i = 0; i < 3; ++i) {
        log_line('I', "t", std::to_string(i).c_str());
    }
    auto entries = fetch();
    CHECK(entries.size() == 3);
    if (entries.size() != 3) return;
    CHECK(std::string(entries[0].message) == "0");
    CHECK(std::string(entries[2].message) == "2");
}

// ---------------------------------------------------------------------------
// Argument handling.
// ---------------------------------------------------------------------------

void get_rejects_nonsense_arguments() {
    start();
    log_line('I', "t", "x");
    log_entry_t entry;
    CHECK(log_buffer_get(nullptr, 10, 0, ESP_LOG_VERBOSE) == 0);
    CHECK(log_buffer_get(&entry, 0, 0, ESP_LOG_VERBOSE) == 0);
    CHECK(log_buffer_get(&entry, -1, 0, ESP_LOG_VERBOSE) == 0);
}

// The hook is the ONLY thing between esp_log and the serial console. If a
// change ever made it capture without forwarding, every device test's
// serial log -- our primary debugging tool for boot failures -- would go
// silent, and nothing else in the suite would notice. Lines the parser
// rejects must be forwarded too.
void capturing_a_line_still_writes_it_to_serial() {
    start();
    char path[] = "/tmp/log_buffer_serial_XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);

    FILE *redirected = std::freopen(path, "w", stdout);
    CHECK(redirected != nullptr);
    log_line('I', "wifi", "stored and forwarded");
    emit("raw output the parser rejects\n");
    std::fflush(stdout);

    std::string captured;
    if (FILE *f = std::fopen(path, "r")) {
        char chunk[512];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
            captured.append(chunk, n);
        }
        std::fclose(f);
    }
    std::remove(path);

    CHECK(captured.find("stored and forwarded") != std::string::npos);
    CHECK(captured.find("raw output the parser rejects") != std::string::npos);
    // ...and the rejected line still is not stored.
    CHECK(log_buffer_count() == 1);
}

void level_characters_round_trip() {
    CHECK(std::string(log_level_char(ESP_LOG_ERROR)) == "E");
    CHECK(std::string(log_level_char(ESP_LOG_WARN)) == "W");
    CHECK(std::string(log_level_char(ESP_LOG_INFO)) == "I");
    CHECK(std::string(log_level_char(ESP_LOG_DEBUG)) == "D");
    CHECK(std::string(log_level_char(ESP_LOG_VERBOSE)) == "V");
    CHECK(std::string(log_level_char(ESP_LOG_NONE)) == "?");
}

// ---------------------------------------------------------------------------

using Test = void (*)();

// Each test forks: the ring, the head and the sequence counter are file-scope
// statics, and init() installs a process-wide hook.
int run_test(const char *name, Test fn) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // The hook forwards every captured line to stdout; a wrap test emits
        // 266 of them. Keep the results readable -- failures go to stderr.
        if (std::freopen("/dev/null", "w", stdout) == nullptr) {
            _exit(2);
        }
        g_failures = 0;
        fn();
        std::fflush(stdout);
        _exit(g_failures == 0 ? 0 : 1);
    }
    if (pid < 0) {
        std::printf("FAIL [%s]: fork failed\n", name);
        return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        std::printf("FAIL [%s]: crashed with signal %d\n", name, WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::printf("FAIL [%s]\n", name);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define TEST(name, fn)                \
    do {                              \
        ++total;                      \
        failed += run_test(name, fn); \
    } while (0)

    TEST("a plain log line is split into level tag and message",
         a_plain_log_line_is_split_into_level_tag_and_message);
    TEST("every level character maps to its level",
         every_level_character_maps_to_its_level);
    TEST("colour escapes are stripped from both ends",
         colour_escapes_are_stripped_from_both_ends);
    TEST("a line that is not a log entry is not stored",
         a_line_that_is_not_a_log_entry_is_not_stored);
    TEST("an over long message is truncated not overflowed",
         an_over_long_message_is_truncated_not_overflowed);
    TEST("an over long tag is truncated not overflowed",
         an_over_long_tag_is_truncated_not_overflowed);
    TEST("a message containing the separator survives intact",
         a_message_containing_the_separator_survives_intact);
    TEST("the uptime stamp comes from the clock",
         the_uptime_stamp_comes_from_the_clock);

    TEST("entries come back oldest first", entries_come_back_oldest_first);
    TEST("a wrapped ring still reads oldest first",
         a_wrapped_ring_still_reads_oldest_first);
    TEST("exactly filling the ring does not wrap",
         exactly_filling_the_ring_does_not_wrap);
    TEST("max count returns the oldest entries first",
         max_count_returns_the_oldest_entries_first);

    TEST("since seq returns only strictly newer entries",
         since_seq_returns_only_strictly_newer_entries);
    TEST("min level keeps entries at least as severe",
         min_level_keeps_entries_at_least_as_severe);
    TEST("the two filters apply together", the_two_filters_apply_together);
    TEST("latest seq at level agrees with the get filter",
         latest_seq_at_level_agrees_with_the_get_filter);
    TEST("latest seq at level is zero when nothing matches",
         latest_seq_at_level_is_zero_when_nothing_matches);
    TEST("latest seq at level survives a wrap",
         latest_seq_at_level_survives_a_wrap);

    TEST("clearing empties the ring but not the sequence",
         clearing_empties_the_ring_but_not_the_sequence);
    TEST("a cleared ring starts writing from the beginning",
         a_cleared_ring_starts_writing_from_the_beginning);

    TEST("get rejects nonsense arguments", get_rejects_nonsense_arguments);
    TEST("capturing a line still writes it to serial",
         capturing_a_line_still_writes_it_to_serial);
    TEST("level characters round trip", level_characters_round_trip);

    std::printf("%s: %d/%d scenarios passed\n", failed == 0 ? "PASS" : "FAIL",
                total - failed, total);
    return failed == 0 ? 0 : 1;
}
