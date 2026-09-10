// Tests for the in-memory NVS fake itself.
//
// The fake is test infrastructure, and infrastructure that lies is worse than
// no infrastructure: every persistence assertion in test_i18n.cpp is only as
// trustworthy as the store underneath it. In particular, a fake that treated
// writes as immediately durable would make "the setting survived a reboot"
// pass for code that never commits.

#include "nvs_fake.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void test_write_is_not_durable_until_commit(void) {
    nvs_fake::reset();
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_u8(h, "k", 7) == ESP_OK);

    // Visible through the same handle...
    uint8_t v = 0;
    CHECK(nvs_get_u8(h, "k", &v) == ESP_OK && v == 7);
    // ...but not yet committed.
    CHECK(!nvs_fake::peek_u8("ns", "k", nullptr));

    CHECK(nvs_commit(h) == ESP_OK);
    CHECK(nvs_fake::peek_u8("ns", "k", &v) && v == 7);
    nvs_close(h);
}

static void test_uncommitted_write_is_lost(void) {
    nvs_fake::reset();
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_u8(h, "k", 9) == ESP_OK);
    nvs_fake::reset_volatile();  // power lost before commit
    CHECK(nvs_commit(h) == ESP_OK);
    CHECK(!nvs_fake::peek_u8("ns", "k", nullptr));
    nvs_close(h);
}

static void test_readonly_open_of_unknown_namespace_fails(void) {
    nvs_fake::reset();
    nvs_handle_t h = 0;
    CHECK(nvs_open("never_written", NVS_READONLY, &h) == ESP_ERR_NVS_NOT_FOUND);

    nvs_fake::seed_u8("written", "k", 1);
    CHECK(nvs_open("written", NVS_READONLY, &h) == ESP_OK);
    nvs_close(h);
}

static void test_readonly_handle_rejects_writes(void) {
    nvs_fake::reset();
    nvs_fake::seed_u8("ns", "k", 1);
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READONLY, &h) == ESP_OK);
    CHECK(nvs_set_u8(h, "k", 2) == ESP_ERR_NVS_READ_ONLY);
    uint8_t v = 0;
    CHECK(nvs_fake::peek_u8("ns", "k", &v) && v == 1);
    nvs_close(h);
}

static void test_namespaces_are_isolated(void) {
    nvs_fake::reset();
    nvs_fake::seed_u8("a", "k", 1);
    nvs_fake::seed_u8("b", "k", 2);
    uint8_t v = 0;
    CHECK(nvs_fake::peek_u8("a", "k", &v) && v == 1);
    CHECK(nvs_fake::peek_u8("b", "k", &v) && v == 2);
}

static void test_type_mismatch_is_not_found(void) {
    nvs_fake::reset();
    nvs_fake::seed_str("ns", "k", "hello");
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READONLY, &h) == ESP_OK);
    uint8_t v = 0;
    CHECK(nvs_get_u8(h, "k", &v) == ESP_ERR_NVS_NOT_FOUND);
    nvs_close(h);
}

static void test_closed_handle_is_rejected(void) {
    nvs_fake::reset();
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);
    nvs_close(h);
    CHECK(nvs_set_u8(h, "k", 1) == ESP_ERR_NVS_INVALID_HANDLE);
    CHECK(nvs_commit(h) == ESP_ERR_NVS_INVALID_HANDLE);
}

static void test_str_size_query_and_buffer(void) {
    nvs_fake::reset();
    nvs_fake::seed_str("ns", "k", "hello");
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READONLY, &h) == ESP_OK);

    size_t len = 0;
    CHECK(nvs_get_str(h, "k", nullptr, &len) == ESP_OK);
    CHECK(len == 6);  // includes the NUL, as IDF does

    char buf[6] = {};
    CHECK(nvs_get_str(h, "k", buf, &len) == ESP_OK);
    CHECK(std::strcmp(buf, "hello") == 0);

    size_t small = 2;
    char tiny[2] = {};
    CHECK(nvs_get_str(h, "k", tiny, &small) == ESP_ERR_NVS_INVALID_LENGTH);
    CHECK(small == 6);  // reports what was needed
    nvs_close(h);
}

static void test_erase(void) {
    nvs_fake::reset();
    nvs_fake::seed_u8("ns", "a", 1);
    nvs_fake::seed_u8("ns", "b", 2);
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_erase_key(h, "a") == ESP_OK);
    CHECK(!nvs_fake::peek_u8("ns", "a", nullptr));
    CHECK(nvs_fake::peek_u8("ns", "b", nullptr));
    CHECK(nvs_erase_all(h) == ESP_OK);
    CHECK(!nvs_fake::peek_u8("ns", "b", nullptr));
    nvs_close(h);
}

static void test_failure_injection_counts_down(void) {
    nvs_fake::reset();
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);

    nvs_fake::fail_next(nvs_fake::Op::SetU8, ESP_ERR_NVS_NOT_ENOUGH_SPACE, 2);
    CHECK(nvs_set_u8(h, "k", 1) == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK(nvs_set_u8(h, "k", 1) == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK(nvs_set_u8(h, "k", 1) == ESP_OK);  // injection exhausted

    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL, -1);  // until cleared
    CHECK(nvs_commit(h) == ESP_FAIL);
    CHECK(nvs_commit(h) == ESP_FAIL);
    nvs_fake::clear_failures();
    CHECK(nvs_commit(h) == ESP_OK);
    nvs_close(h);
}

static void test_call_counting_detects_a_path_that_never_wrote(void) {
    nvs_fake::reset();
    CHECK(nvs_fake::call_count(nvs_fake::Op::SetU8) == 0);
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_u8(h, "k", 1) == ESP_OK);
    CHECK(nvs_fake::call_count(nvs_fake::Op::SetU8) == 1);
    nvs_close(h);
}

static void test_reset_clears_everything(void) {
    nvs_fake::reset();
    nvs_fake::seed_u8("ns", "k", 5);
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL, -1);
    nvs_fake::reset();
    CHECK(!nvs_fake::peek_u8("ns", "k", nullptr));
    CHECK(nvs_fake::call_count(nvs_fake::Op::Open) == 0);
    nvs_handle_t h = 0;
    CHECK(nvs_open("ns", NVS_READWRITE, &h) == ESP_OK);  // injection cleared too
    nvs_close(h);
}

static void test_flash_erase_drops_all_namespaces(void) {
    nvs_fake::reset();
    nvs_fake::seed_u8("a", "k", 1);
    nvs_fake::seed_u8("b", "k", 2);
    CHECK(nvs_flash_erase() == ESP_OK);
    CHECK(!nvs_fake::peek_u8("a", "k", nullptr));
    CHECK(!nvs_fake::peek_u8("b", "k", nullptr));
}

int main(void) {
    test_write_is_not_durable_until_commit();
    test_uncommitted_write_is_lost();
    test_readonly_open_of_unknown_namespace_fails();
    test_readonly_handle_rejects_writes();
    test_namespaces_are_isolated();
    test_type_mismatch_is_not_found();
    test_closed_handle_is_rejected();
    test_str_size_query_and_buffer();
    test_erase();
    test_failure_injection_counts_down();
    test_call_counting_detects_a_path_that_never_wrote();
    test_reset_clears_everything();
    test_flash_erase_drops_all_namespaces();

    if (g_failures) {
        std::printf("%d nvs_fake check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("nvs_fake: all checks passed\n");
    return 0;
}
