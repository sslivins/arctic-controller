// Native unit tests for main/auth_manager.cpp -- the two things the device
// suite structurally cannot reach: persistence failures and concurrency.
//
// What existed before: tests/api/test_auth.py, which logs in over HTTP against
// the one physical controller. It exercises the happy path only. It cannot
// make NVS fail, so every "return false when the write fails" branch in this
// file was unreachable; and it issues requests one at a time, so the
// integration-token mutex and the generation guard -- whose entire purpose is
// to serialise concurrent writers -- were never contended even once.
//
// The claims under test are the ones the code makes about itself:
//   * "a silent failure here would leave the running config and the persisted
//     config disagreeing" (auth_manager.cpp, save_to_nvs)
//   * a token that could not be persisted is not accepted afterwards
//   * a failed begin_control_write() gives the lock back (a leak here wedges
//     every later control write on the device, forever)
//   * "zeroing cannot match a real SHA-256 of a password" (hash_password)
//
// ISOLATION: auth_manager.cpp keeps its state in a file-scope `state` struct
// and creates its mutex once. Running two tests in one process therefore lets
// the first contaminate the second. Each test runs in its own forked child, so
// statics start cold -- the same reason test_history_storage.cpp forks.
//
// Refs #217 (T17).

#include "auth_manager.h"

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_fake.h"
#include "psa/crypto.h"

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace {

constexpr const char *NS = "auth";
constexpr const char *KEY_PASS_HASH = "pass_hash";
constexpr const char *KEY_INTEGRATION_HASH = "ha_hash";
constexpr const char *USER = "arctic";
constexpr const char *GOOD_PASS = "correct horse";
constexpr const char *OTHER_PASS = "battery staple";

// A booted device with credentials the tests can log in with.
void boot_with_credentials() {
    nvs_fake::reset();
    random_fake::reset();
    psa_fake::reset();
    auth_mgr_init();
    CHECK(auth_mgr_set_credentials(USER, GOOD_PASS));
    nvs_fake::clear_failures();
}

std::string login_or_empty(const char *password) {
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    if (!auth_mgr_login(USER, password, token)) {
        return std::string();
    }
    return std::string(token);
}

// ---------------------------------------------------------------------------
// Crypto. The module stores only digests, so a broken hash makes every other
// assertion in this file meaningless. Pin it to the published vectors.
// ---------------------------------------------------------------------------

std::string hex(const uint8_t *bytes, size_t len) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0F];
    }
    return out;
}

void sha256_matches_the_published_vectors() {
    uint8_t out[32];
    size_t len = 0;
    CHECK(psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)"abc", 3, out,
                           sizeof(out), &len) == PSA_SUCCESS);
    CHECK(len == 32);
    CHECK(hex(out, 32) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    CHECK(psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)"", 0, out,
                           sizeof(out), &len) == PSA_SUCCESS);
    CHECK(hex(out, 32) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ---------------------------------------------------------------------------
// Persistence failures.
// ---------------------------------------------------------------------------

// The reason set_credentials returns a bool at all: the caller has to be able
// to tell the user the new password did not stick.
void a_credential_write_that_fails_is_reported() {
    boot_with_credentials();

    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    const bool saved = auth_mgr_set_credentials(USER, OTHER_PASS);
    CHECK(!saved);
    // It must have tried; a pass because nothing was written would be worthless.
    CHECK(nvs_fake::call_count(nvs_fake::Op::Commit) > 0);
}

// The divergence that failure leaves behind: the new password is live in RAM
// but flash still holds the old one, so a reboot silently reverts it. This is
// what "the running config and the persisted config disagreeing" means, and it
// is a real behaviour of the shipped firmware -- documented here so a change
// to it is a deliberate one.
void a_failed_credential_write_leaves_ram_ahead_of_flash() {
    boot_with_credentials();
    std::vector<uint8_t> before;
    CHECK(nvs_fake::peek_blob(NS, KEY_PASS_HASH, &before));

    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK(!auth_mgr_set_credentials(USER, OTHER_PASS));
    nvs_fake::clear_failures();

    // In RAM the change took effect...
    CHECK(login_or_empty(GOOD_PASS).empty());
    CHECK(!login_or_empty(OTHER_PASS).empty());

    // ...but flash never moved, so the next boot resurrects the old password.
    std::vector<uint8_t> after;
    CHECK(nvs_fake::peek_blob(NS, KEY_PASS_HASH, &after));
    CHECK(before == after);
}

// Fail-closed: a token whose hash could not be stored must not be usable. If
// it were, the integration would work until the next reboot and then stop,
// which is the worst possible failure mode to debug.
void a_token_that_could_not_be_persisted_is_not_accepted() {
    boot_with_credentials();
    const uint32_t generation_before = auth_mgr_get_integration_generation();

    char token[AUTH_INTEGRATION_TOKEN_LEN + 1];
    std::memset(token, 'x', sizeof(token));
    nvs_fake::fail_next(nvs_fake::Op::SetBlob, ESP_FAIL);
    CHECK(!auth_mgr_issue_integration_token(token));

    CHECK(token[0] == '\0');
    CHECK(!auth_mgr_has_integration_token());
    CHECK(!auth_mgr_validate_integration_token(token));
    // A bumped generation would invalidate live control writes for a token
    // that was never issued.
    CHECK(auth_mgr_get_integration_generation() == generation_before);
    CHECK(!nvs_fake::peek_blob(NS, KEY_INTEGRATION_HASH, nullptr));
}

void a_commit_failure_also_rejects_the_token() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK(!auth_mgr_issue_integration_token(token));
    CHECK(!auth_mgr_has_integration_token());
}

// The mirror image: a revoke that could not be persisted must NOT report
// success, because the caller would tell the user the token is dead while the
// device still honours it.
void a_revoke_that_could_not_be_persisted_reports_failure() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));

    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK(!auth_mgr_revoke_integration_token());
    nvs_fake::clear_failures();

    // Still armed, and still the same generation: nothing changed.
    CHECK(auth_mgr_has_integration_token());
    CHECK(auth_mgr_validate_integration_token(token));
}

void a_revoke_that_succeeds_invalidates_the_token() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));
    const uint32_t gen = auth_mgr_get_integration_generation();

    CHECK(auth_mgr_revoke_integration_token());
    CHECK(!auth_mgr_has_integration_token());
    CHECK(!auth_mgr_validate_integration_token(token));
    CHECK(auth_mgr_get_integration_generation() == gen + 1);
    CHECK(!nvs_fake::peek_blob(NS, KEY_INTEGRATION_HASH, nullptr));
}

// Revoking when nothing is armed is a no-op, not an error: NOT_FOUND from
// erase_key is mapped to OK on purpose.
void revoking_an_absent_token_succeeds() {
    boot_with_credentials();
    CHECK(auth_mgr_revoke_integration_token());
    CHECK(!auth_mgr_has_integration_token());
}

// ---------------------------------------------------------------------------
// Hash failures. Injectable only from the host; on the device this branch is
// dead code that has never once executed.
// ---------------------------------------------------------------------------

// The dangerous case. hash_password zeroes its output on failure and claims
// "zeroing cannot match a real SHA-256 of a password" -- true only if the
// STORED hash is a real one. If hashing fails while the password is being set,
// the stored hash is all zeros; if it then fails again at login, the input
// hash is all zeros too, and the comparison succeeds for ANY password.
void a_hash_failure_never_authenticates() {
    nvs_fake::reset();
    psa_fake::reset();
    auth_mgr_init();

    psa_fake::fail_next_hash(1);
    // Setting a password whose hash could not be computed must not leave the
    // account in a state where anything logs in.
    (void)auth_mgr_set_credentials(USER, GOOD_PASS);
    nvs_fake::clear_failures();

    psa_fake::fail_next_hash(1);
    CHECK(login_or_empty("anything at all").empty());

    psa_fake::reset();
    CHECK(login_or_empty("anything at all").empty());
    CHECK(login_or_empty(GOOD_PASS).empty());
}

void a_hash_failure_during_login_rejects_a_correct_password() {
    boot_with_credentials();
    psa_fake::fail_next_hash(1);
    CHECK(login_or_empty(GOOD_PASS).empty());
    // And recovers once hashing works again.
    CHECK(!login_or_empty(GOOD_PASS).empty());
}

void a_hash_failure_while_issuing_a_token_is_not_accepted() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1];
    std::memset(token, 'x', sizeof(token));
    psa_fake::fail_next_hash(1);
    // Issuing anyway would hand out a token that is dead on arrival, and would
    // store a zeroed digest that a later hash failure makes universally valid.
    CHECK(!auth_mgr_issue_integration_token(token));
    psa_fake::reset();
    CHECK(token[0] == '\0');
    CHECK(!auth_mgr_has_integration_token());
    CHECK(!nvs_fake::peek_blob(NS, KEY_INTEGRATION_HASH, nullptr));
}

// The other half of the same fail-open: even with a good stored digest, a hash
// failure while VALIDATING must reject rather than compare zeroes.
void a_hash_failure_while_validating_rejects_the_token() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));
    CHECK(auth_mgr_validate_integration_token(token));

    psa_fake::fail_next_hash(1);
    CHECK(!auth_mgr_validate_integration_token(token));
    psa_fake::reset();
    CHECK(auth_mgr_validate_integration_token(token));
}

// A device that ran the firmware BEFORE the fail-closed fix can have a zeroed
// digest sitting in flash: the old hash_password zeroed its output on failure
// and the caller stored it regardless. Upgrading does not clean that up. So
// the validator must still refuse a token it could not hash, even though the
// caller-side fix means new devices can never reach this state.
void a_zeroed_digest_left_by_old_firmware_never_validates() {
    nvs_fake::reset();
    psa_fake::reset();
    random_fake::reset();
    nvs_fake::seed_blob(NS, KEY_INTEGRATION_HASH, std::vector<uint8_t>(32, 0));
    auth_mgr_init();
    CHECK(auth_mgr_has_integration_token());

    std::string any(AUTH_INTEGRATION_TOKEN_LEN, 'a');
    // Hashing works: a real digest cannot be all zeros, so this fails anyway.
    CHECK(!auth_mgr_validate_integration_token(any.c_str()));
    // Hashing fails: without the guard the zeroed input would match the
    // zeroed stored digest and admit an attacker-chosen token.
    psa_fake::fail_next_hash(1);
    CHECK(!auth_mgr_validate_integration_token(any.c_str()));
}

// ---------------------------------------------------------------------------
// Sessions.
// ---------------------------------------------------------------------------

void a_wrong_password_creates_no_session() {
    boot_with_credentials();
    CHECK(login_or_empty(OTHER_PASS).empty());
    CHECK(!auth_mgr_validate_session(""));
    // All four slots are still free.
    for (int i = 0; i < AUTH_MAX_SESSIONS; ++i) {
        CHECK(!login_or_empty(GOOD_PASS).empty());
    }
}

// Not exhaustion: find_free_session() evicts the least recently used slot once
// all four are taken, so a fifth login succeeds and the oldest session ends.
// Asserted here because the alternative -- refusing the fifth login -- would
// lock a user out of their own controller, and the difference is invisible
// over HTTP unless five sessions are held open at once.
void a_fifth_login_evicts_the_oldest_session() {
    boot_with_credentials();
    std::vector<std::string> tokens;
    for (int i = 0; i < AUTH_MAX_SESSIONS; ++i) {
        std::string t = login_or_empty(GOOD_PASS);
        CHECK(!t.empty());
        tokens.push_back(t);
    }
    // Every issued token is distinct: a collision would let one browser's
    // logout end another's session.
    CHECK(std::set<std::string>(tokens.begin(), tokens.end()).size() ==
          tokens.size());
    for (const auto &t : tokens) {
        CHECK(auth_mgr_validate_session(t.c_str()));
    }

    std::string fifth = login_or_empty(GOOD_PASS);
    CHECK(!fifth.empty());
    CHECK(auth_mgr_validate_session(fifth.c_str()));
    // Exactly one session was displaced, and it is the oldest.
    CHECK(!auth_mgr_validate_session(tokens[0].c_str()));
    for (size_t i = 1; i < tokens.size(); ++i) {
        CHECK(auth_mgr_validate_session(tokens[i].c_str()));
    }
}

void a_logged_out_slot_is_reused_before_anything_is_evicted() {
    boot_with_credentials();
    std::vector<std::string> tokens;
    for (int i = 0; i < AUTH_MAX_SESSIONS; ++i) {
        tokens.push_back(login_or_empty(GOOD_PASS));
    }
    auth_mgr_logout(tokens[1].c_str());
    CHECK(!auth_mgr_validate_session(tokens[1].c_str()));

    CHECK(!login_or_empty(GOOD_PASS).empty());
    // The free slot absorbed the new session, so nothing else was evicted.
    CHECK(auth_mgr_validate_session(tokens[0].c_str()));
    CHECK(auth_mgr_validate_session(tokens[2].c_str()));
    CHECK(auth_mgr_validate_session(tokens[3].c_str()));
}

// Changing the password must end every existing session, or a stolen cookie
// outlives the reason the user changed it.
void changing_credentials_invalidates_live_sessions() {
    boot_with_credentials();
    std::string a = login_or_empty(GOOD_PASS);
    std::string b = login_or_empty(GOOD_PASS);
    CHECK(!a.empty());
    CHECK(!b.empty());

    CHECK(auth_mgr_set_credentials(USER, OTHER_PASS));
    CHECK(!auth_mgr_validate_session(a.c_str()));
    CHECK(!auth_mgr_validate_session(b.c_str()));
    CHECK(login_or_empty(GOOD_PASS).empty());
    CHECK(!login_or_empty(OTHER_PASS).empty());
}

// ...including when the write failed. The sessions are dropped either way; if
// they were kept only on the success path, a failed save would leave sessions
// authenticated against a password the user believes is gone.
void a_failed_credential_write_still_invalidates_sessions() {
    boot_with_credentials();
    std::string a = login_or_empty(GOOD_PASS);
    CHECK(!a.empty());

    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK(!auth_mgr_set_credentials(USER, OTHER_PASS));
    CHECK(!auth_mgr_validate_session(a.c_str()));
}

void an_unknown_session_token_is_rejected() {
    boot_with_credentials();
    std::string good = login_or_empty(GOOD_PASS);
    CHECK(!good.empty());
    CHECK(!auth_mgr_validate_session(nullptr));
    CHECK(!auth_mgr_validate_session(""));
    std::string tampered = good;
    tampered[0] = (tampered[0] == 'a') ? 'b' : 'a';
    CHECK(!auth_mgr_validate_session(tampered.c_str()));
    CHECK(auth_mgr_validate_session(good.c_str()));
}

void logout_all_ends_every_session() {
    boot_with_credentials();
    std::vector<std::string> tokens;
    for (int i = 0; i < AUTH_MAX_SESSIONS; ++i) {
        tokens.push_back(login_or_empty(GOOD_PASS));
    }
    auth_mgr_logout_all();
    for (const auto &t : tokens) {
        CHECK(!auth_mgr_validate_session(t.c_str()));
    }
    CHECK(!login_or_empty(GOOD_PASS).empty());
}

// ---------------------------------------------------------------------------
// Token uniqueness under a hostile RNG.
// ---------------------------------------------------------------------------

// generate_random_hex maps esp_random() onto hex digits. Forcing the RNG to
// repeat proves the tests would notice a token collision rather than relying
// on 2^-128 luck to hide one.
void a_repeating_rng_produces_a_repeating_token() {
    boot_with_credentials();
    char first[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    char second[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};

    random_fake::force_next(7, AUTH_INTEGRATION_TOKEN_LEN);
    CHECK(auth_mgr_issue_integration_token(first));
    random_fake::force_next(7, AUTH_INTEGRATION_TOKEN_LEN);
    CHECK(auth_mgr_issue_integration_token(second));
    CHECK(std::strcmp(first, second) == 0);

    // And the ordinary sequence does not repeat.
    char third[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    char fourth[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(third));
    CHECK(auth_mgr_issue_integration_token(fourth));
    CHECK(std::strcmp(third, fourth) != 0);
}

void a_token_of_the_wrong_length_is_rejected_before_hashing() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));

    const int hashes = psa_fake::hash_calls();
    CHECK(!auth_mgr_validate_integration_token(nullptr));
    CHECK(!auth_mgr_validate_integration_token(""));
    std::string truncated(token, AUTH_INTEGRATION_TOKEN_LEN - 1);
    CHECK(!auth_mgr_validate_integration_token(truncated.c_str()));
    CHECK(psa_fake::hash_calls() == hashes);
}

// ---------------------------------------------------------------------------
// Concurrency. The device suite issues one request at a time and can never
// reach any of this.
// ---------------------------------------------------------------------------

constexpr int THREADS = 8;
constexpr int ROUNDS = 40;

// N threads issuing tokens at once. The invariant is that the module is never
// observed half-updated: whatever token is armed at the end must validate, and
// the generation must have advanced exactly once per successful issue.
void concurrent_issues_leave_a_consistent_state() {
    boot_with_credentials();
    sem_fake::reset_totals();

    std::atomic<int> issued{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&issued]() {
            for (int i = 0; i < ROUNDS; ++i) {
                char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
                if (auth_mgr_issue_integration_token(token)) {
                    issued.fetch_add(1);
                    // The token this thread was handed must be a real one:
                    // 64 hex chars, never a partially-written buffer.
                    if (std::strlen(token) != AUTH_INTEGRATION_TOKEN_LEN) {
                        std::abort();
                    }
                }
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    CHECK(issued.load() == THREADS * ROUNDS);
    CHECK(auth_mgr_get_integration_generation() == (uint32_t)issued.load());
    CHECK(auth_mgr_has_integration_token());
    // A test that never contended is not a concurrency test.
    CHECK(sem_fake::total_contended().load() > 0);
    // Every take was matched: no path leaked the lock.
    CHECK(sem_fake::total_takes().load() == sem_fake::total_gives().load());
}

// Readers racing writers. validate() takes the spinlock, not the mutex, so it
// must never see a hash from one generation with the flag from another.
void validation_races_issue_without_tearing() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&stop, &reads]() {
            while (!stop.load()) {
                uint32_t gen = 0;
                // A stale token must simply fail; it must never crash, and it
                // must never come back valid with a generation of zero.
                char stale[AUTH_INTEGRATION_TOKEN_LEN + 1];
                std::memset(stale, 'a', AUTH_INTEGRATION_TOKEN_LEN);
                stale[AUTH_INTEGRATION_TOKEN_LEN] = '\0';
                if (auth_mgr_validate_integration_token_with_generation(stale,
                                                                       &gen)) {
                    std::abort();
                }
                (void)auth_mgr_get_integration_generation();
                reads.fetch_add(1);
            }
        });
    }

    for (int i = 0; i < 200; ++i) {
        char fresh[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
        CHECK(auth_mgr_issue_integration_token(fresh));
        CHECK(auth_mgr_validate_integration_token(fresh));
    }
    stop.store(true);
    for (auto &th : readers) {
        th.join();
    }
    CHECK(reads.load() > 0);
}

// The generation guard's whole job: a control write authorised against a token
// that has since been replaced must be refused.
void a_stale_generation_cannot_begin_a_control_write() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));
    const uint32_t stale = auth_mgr_get_integration_generation();

    CHECK(auth_mgr_issue_integration_token(token));
    CHECK(auth_mgr_get_integration_generation() != stale);

    sem_fake::reset_totals();
    CHECK(!auth_mgr_begin_control_write(stale));
    // The refusal path must give the lock back. If it does not, the next
    // begin_control_write on the device blocks forever -- a wedge that would
    // need a power cycle to clear, and that no HTTP test could ever see.
    CHECK(sem_fake::total_takes().load() == sem_fake::total_gives().load());

    CHECK(auth_mgr_begin_control_write(auth_mgr_get_integration_generation()));
    auth_mgr_end_control_write();
    CHECK(sem_fake::total_takes().load() == sem_fake::total_gives().load());
}

void a_control_write_is_refused_once_the_token_is_revoked() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));
    const uint32_t gen = auth_mgr_get_integration_generation();

    CHECK(auth_mgr_revoke_integration_token());
    sem_fake::reset_totals();
    CHECK(!auth_mgr_begin_control_write(gen));
    CHECK(sem_fake::total_takes().load() == sem_fake::total_gives().load());
}

// A control write holds the mutex, so a concurrent token change must wait for
// it rather than swapping the token mid-write.
void a_token_change_waits_for_an_in_flight_control_write() {
    boot_with_credentials();
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {0};
    CHECK(auth_mgr_issue_integration_token(token));

    CHECK(auth_mgr_begin_control_write(auth_mgr_get_integration_generation()));

    std::atomic<bool> revoked{false};
    std::thread revoker([&revoked]() {
        auth_mgr_revoke_integration_token();
        revoked.store(true);
    });

    // While the write is in flight the revoke must be blocked. This is a
    // negative observation over a window, so it is deliberately generous: a
    // slow machine must not turn it red, but a missing mutex would let the
    // revoke through immediately every time.
    for (int i = 0; i < 20 && !revoked.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!revoked.load());
    CHECK(auth_mgr_has_integration_token());

    auth_mgr_end_control_write();
    revoker.join();
    CHECK(revoked.load());
    CHECK(!auth_mgr_has_integration_token());
}

// Many threads logging in and out at once. The session table has no lock, so
// the point of this test is the end state: however the interleavings land, the
// table must not be left wedged or inconsistent -- a fresh login must still
// work and validate, and no more than AUTH_MAX_SESSIONS may be live.
//
// (On the device these calls run on the single httpd task today, so this
// guards a property the code does not enforce with a lock. If a second HTTP
// worker is ever enabled, this is the test that should start failing.)
void a_login_storm_leaves_the_session_table_usable() {
    boot_with_credentials();

    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&succeeded]() {
            for (int i = 0; i < 20; ++i) {
                char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
                if (auth_mgr_login(USER, GOOD_PASS, token)) {
                    succeeded.fetch_add(1);
                    (void)auth_mgr_validate_session(token);
                    auth_mgr_logout(token);
                }
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    CHECK(succeeded.load() > 0);

    auth_mgr_logout_all();
    std::string fresh = login_or_empty(GOOD_PASS);
    CHECK(!fresh.empty());
    CHECK(auth_mgr_validate_session(fresh.c_str()));
    CHECK(login_or_empty(OTHER_PASS).empty());
}

// ---------------------------------------------------------------------------
// Mandatory-auth invariants (auth_manager.cpp:265-270, :295-300).
// ---------------------------------------------------------------------------

void authentication_cannot_be_turned_off() {
    boot_with_credentials();
    CHECK(auth_mgr_web_auth_enabled());
    CHECK(auth_mgr_api_auth_enabled());

    auth_mgr_set_web_auth_enabled(false);
    auth_mgr_set_api_auth_enabled(false);
    CHECK(auth_mgr_web_auth_enabled());
    CHECK(auth_mgr_api_auth_enabled());

    // Not even by seeding flash with the disabled flags a previous firmware
    // might have written.
    nvs_fake::seed_u8(NS, "web_en", 0);
    nvs_fake::seed_u8(NS, "api_en", 0);
    auth_mgr_init();
    CHECK(auth_mgr_web_auth_enabled());
    CHECK(auth_mgr_api_auth_enabled());
}

void factory_credentials_are_flagged_for_change() {
    nvs_fake::reset();
    psa_fake::reset();
    auth_mgr_init();
    CHECK(auth_mgr_credentials_change_required());
    CHECK(auth_mgr_set_credentials(USER, GOOD_PASS));
    CHECK(!auth_mgr_credentials_change_required());
}

void an_api_key_is_regenerated_and_the_old_one_stops_working() {
    boot_with_credentials();
    char first[AUTH_API_KEY_LEN + 1] = {0};
    CHECK(auth_mgr_regenerate_api_key(first));
    CHECK(std::strlen(first) == AUTH_API_KEY_LEN);
    CHECK(auth_mgr_validate_api_key(first));

    char second[AUTH_API_KEY_LEN + 1] = {0};
    CHECK(auth_mgr_regenerate_api_key(second));
    CHECK(std::strcmp(first, second) != 0);
    CHECK(!auth_mgr_validate_api_key(first));
    CHECK(auth_mgr_validate_api_key(second));
    CHECK(!auth_mgr_validate_api_key(""));
    CHECK(!auth_mgr_validate_api_key(nullptr));
}

// ---------------------------------------------------------------------------

using Test = void (*)();

// Each test runs in its own child: auth_manager's `state` is a file-scope
// static and its mutex is created once, so in-process reuse would let tests
// contaminate each other.
int run_test(const char *name, Test fn) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
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

#define TEST(name, fn)          \
    do {                        \
        ++total;                \
        failed += run_test(name, fn); \
    } while (0)

    TEST("sha256 matches the published vectors",
         sha256_matches_the_published_vectors);

    TEST("a credential write that fails is reported",
         a_credential_write_that_fails_is_reported);
    TEST("a failed credential write leaves ram ahead of flash",
         a_failed_credential_write_leaves_ram_ahead_of_flash);
    TEST("a token that could not be persisted is not accepted",
         a_token_that_could_not_be_persisted_is_not_accepted);
    TEST("a commit failure also rejects the token",
         a_commit_failure_also_rejects_the_token);
    TEST("a revoke that could not be persisted reports failure",
         a_revoke_that_could_not_be_persisted_reports_failure);
    TEST("a revoke that succeeds invalidates the token",
         a_revoke_that_succeeds_invalidates_the_token);
    TEST("revoking an absent token succeeds", revoking_an_absent_token_succeeds);

    TEST("a hash failure never authenticates", a_hash_failure_never_authenticates);
    TEST("a hash failure during login rejects a correct password",
         a_hash_failure_during_login_rejects_a_correct_password);
    TEST("a hash failure while issuing a token is not accepted",
         a_hash_failure_while_issuing_a_token_is_not_accepted);
    TEST("a hash failure while validating rejects the token",
         a_hash_failure_while_validating_rejects_the_token);
    TEST("a zeroed digest left by old firmware never validates",
         a_zeroed_digest_left_by_old_firmware_never_validates);

    TEST("a wrong password creates no session", a_wrong_password_creates_no_session);
    TEST("a fifth login evicts the oldest session",
         a_fifth_login_evicts_the_oldest_session);
    TEST("a logged out slot is reused before anything is evicted",
         a_logged_out_slot_is_reused_before_anything_is_evicted);
    TEST("changing credentials invalidates live sessions",
         changing_credentials_invalidates_live_sessions);
    TEST("a failed credential write still invalidates sessions",
         a_failed_credential_write_still_invalidates_sessions);
    TEST("an unknown session token is rejected",
         an_unknown_session_token_is_rejected);
    TEST("logout all ends every session", logout_all_ends_every_session);

    TEST("a repeating rng produces a repeating token",
         a_repeating_rng_produces_a_repeating_token);
    TEST("a token of the wrong length is rejected before hashing",
         a_token_of_the_wrong_length_is_rejected_before_hashing);

    TEST("concurrent issues leave a consistent state",
         concurrent_issues_leave_a_consistent_state);
    TEST("validation races issue without tearing",
         validation_races_issue_without_tearing);
    TEST("a stale generation cannot begin a control write",
         a_stale_generation_cannot_begin_a_control_write);
    TEST("a control write is refused once the token is revoked",
         a_control_write_is_refused_once_the_token_is_revoked);
    TEST("a token change waits for an in flight control write",
         a_token_change_waits_for_an_in_flight_control_write);
    TEST("a login storm leaves the session table usable",
         a_login_storm_leaves_the_session_table_usable);

    TEST("authentication cannot be turned off", authentication_cannot_be_turned_off);
    TEST("factory credentials are flagged for change",
         factory_credentials_are_flagged_for_change);
    TEST("an api key is regenerated and the old one stops working",
         an_api_key_is_regenerated_and_the_old_one_stops_working);

    std::printf("%s: %d/%d scenarios passed\n", failed == 0 ? "PASS" : "FAIL",
                total - failed, total);
    return failed == 0 ? 0 : 1;
}
