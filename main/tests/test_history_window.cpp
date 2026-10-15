// Host tests for history_window: cycle-history step and re-centre targets.

#include <cstdio>
#include <cstdlib>

#include "history_window.h"

static int g_failures = 0;

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const long long va = (long long)(a), vb = (long long)(b);          \
        if (va != vb) {                                                    \
            std::fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", __FILE__, \
                         __LINE__, #a, va, vb);                            \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static constexpr uint32_t W = 8 * 3600;
static constexpr uint32_t LATEST = 1760500000;

int main() {
    // One-hour steps.
    CHECK_EQ(history_window_shift(LATEST - W, -3600, W, LATEST), LATEST - W - 3600);
    CHECK_EQ(history_window_shift(LATEST - W, 3600, W, LATEST), LATEST - W + 3600);
    // Never past the latest data, never before one window from zero.
    CHECK_EQ(history_window_shift(LATEST - 600, 3600, W, LATEST), LATEST);
    CHECK_EQ(history_window_shift(W + 100, -3600, W, LATEST), W);

    // Re-centre: the tapped moment lands mid-window, snapped to 15 min.
    const uint32_t t = 1760400000;  // a multiple of 900 s
    CHECK_EQ(history_window_center(t, W, 900, LATEST), t + W / 2);
    CHECK_EQ(history_window_center(t + 100, W, 900, LATEST), t + W / 2);
    CHECK_EQ(history_window_center(t + 500, W, 900, LATEST), t + W / 2 + 900);
    // Near now, the window pins to the latest data instead.
    CHECK_EQ(history_window_center(LATEST - 600, W, 900, LATEST), LATEST);
    // Near zero, the window can't start before zero.
    CHECK_EQ(history_window_center(60, W, 900, LATEST), W);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("history_window: all checks passed");
    return EXIT_SUCCESS;
}
