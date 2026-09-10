/*
 * Implementation of the steerable wall clock. See time_fake.h.
 *
 * Defining time() here shadows libc's: the linker resolves it from this object
 * file, which is listed before -lc. The value lives in plain (non-shared)
 * memory on purpose -- each forked "reboot" phase starts from whatever the
 * parent last set, which is the same thing a real device does when its RTC is
 * not backed up.
 */

#include "time_fake.h"

namespace {
time_t g_now = time_fake::DEFAULT_NOW;
}

extern "C" time_t time(time_t *out) {
    if (out != nullptr) {
        *out = g_now;
    }
    return g_now;
}

namespace time_fake {

void set(time_t now) { g_now = now; }

time_t get() { return g_now; }

void reset() { g_now = DEFAULT_NOW; }

}  // namespace time_fake
