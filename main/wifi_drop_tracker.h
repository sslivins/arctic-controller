/*
 * Arctic Heat Pump Controller
 * WiFi drop tracker - decides which WiFi outages reach the event log.
 *
 * Pure logic over a caller-supplied millisecond clock, so it is host-tested
 * (main/tests/test_wifi_drop_tracker.cpp). wifi_drops.cpp feeds it from the
 * WiFi event handler and drains its actions from the supervisor task.
 *
 * Policy:
 *   - An outage starts at the first disconnect after the link was up and ends
 *     when an IP is held again. Retry failures in between are the same outage.
 *   - Disconnects the controller asked for (supervisor bounce, user changing
 *     networks) are not drops.
 *   - LONG_MS or longer: one LOST event (payload = disconnect reason) once it
 *     crosses the threshold, and one RESTORED event (payload = seconds down)
 *     when it ends. A device that never comes back still has its LOST event.
 *   - Shorter drops are only counted. UNSTABLE_COUNT of them within
 *     UNSTABLE_WINDOW_MS log one UNSTABLE event (payload = count). Further
 *     short drops are then held for a window; if that window also reaches
 *     UNSTABLE_COUNT it logs one more UNSTABLE with its count, so a flapping
 *     link writes at most one event per window.
 */
#pragma once

#include <stdint.h>

enum class WifiDropAction : uint8_t {
    LOST,      // payload: wifi_err_reason_t of the disconnect
    RESTORED,  // payload: outage length in seconds
    UNSTABLE,  // payload: short drops in the window
};

struct WifiDropEvent {
    WifiDropAction action;
    uint32_t payload;
};

struct WifiDropStats {
    uint32_t drops;        // outages since boot (excluding ones we asked for)
    uint32_t short_drops;  // ended in under LONG_MS
    uint32_t long_drops;   // reached LONG_MS (including one still in progress)
    uint16_t last_reason;  // reason of the last outage, 0 if none
    bool down;             // an outage is in progress
    uint32_t down_s;       // its length so far, 0 if none
};

class WifiDropTracker {
public:
    static constexpr int64_t LONG_MS = 60000;
    static constexpr int64_t UNSTABLE_WINDOW_MS = 3600000;
    static constexpr int UNSTABLE_COUNT = 3;
    static constexpr int MAX_PENDING = 4;

    // The link lost association. `local` = the controller disconnected it.
    void on_down(int64_t now_ms, uint16_t reason, bool local) {
        if (!ever_up_ || down_) return;
        down_ = true;
        down_local_ = local;
        lost_logged_ = false;
        down_since_ms_ = now_ms;
        if (local) return;
        reason_ = reason;
        stats_.drops++;
        stats_.last_reason = reason;
    }

    // The link is up and holding an IP.
    void on_up(int64_t now_ms) {
        ever_up_ = true;
        if (!down_) return;
        down_ = false;
        if (down_local_) return;
        const int64_t dur = now_ms - down_since_ms_;
        if (lost_logged_ || dur >= LONG_MS) {
            if (!lost_logged_) {
                push(WifiDropAction::LOST, reason_);
                stats_.long_drops++;
            }
            push(WifiDropAction::RESTORED, (uint32_t)(dur / 1000));
        } else {
            stats_.short_drops++;
            if (quiet_until_ms_ != 0) quiet_drops_++;
            else add_short(now_ms);
        }
    }

    // Emit due actions into out[] (oldest first); returns how many.
    int poll(int64_t now_ms, WifiDropEvent* out, int max_out) {
        if (down_ && !down_local_ && !lost_logged_ &&
            now_ms - down_since_ms_ >= LONG_MS) {
            push(WifiDropAction::LOST, reason_);
            lost_logged_ = true;
            stats_.long_drops++;
        }
        if (quiet_until_ms_ != 0 && now_ms >= quiet_until_ms_) {
            if (quiet_drops_ >= UNSTABLE_COUNT) {
                report_unstable(now_ms, quiet_drops_);
            } else {
                quiet_until_ms_ = 0;
            }
            quiet_drops_ = 0;
        }
        prune(now_ms);
        if (quiet_until_ms_ == 0 && short_count_ >= UNSTABLE_COUNT) {
            report_unstable(now_ms, (uint32_t)short_count_);
            short_count_ = 0;
        }
        int n = 0;
        while (n < max_out && pending_count_ > 0) {
            out[n++] = pending_[0];
            for (int i = 1; i < pending_count_; i++) pending_[i - 1] = pending_[i];
            pending_count_--;
        }
        return n;
    }

    WifiDropStats stats(int64_t now_ms) const {
        WifiDropStats s = stats_;
        s.down = down_ && !down_local_;
        s.down_s = s.down ? (uint32_t)((now_ms - down_since_ms_) / 1000) : 0;
        return s;
    }

private:
    void push(WifiDropAction a, uint32_t payload) {
        if (pending_count_ == MAX_PENDING) {
            // Never expected (poll runs every few seconds); keep the newest.
            for (int i = 1; i < pending_count_; i++) pending_[i - 1] = pending_[i];
            pending_count_--;
        }
        pending_[pending_count_++] = WifiDropEvent{a, payload};
    }

    void report_unstable(int64_t now_ms, uint32_t count) {
        push(WifiDropAction::UNSTABLE, count);
        quiet_until_ms_ = now_ms + UNSTABLE_WINDOW_MS;
    }

    void add_short(int64_t now_ms) {
        if (short_count_ == UNSTABLE_COUNT) {
            for (int i = 1; i < short_count_; i++) short_ms_[i - 1] = short_ms_[i];
            short_count_--;
        }
        short_ms_[short_count_++] = now_ms;
    }

    void prune(int64_t now_ms) {
        int keep = 0;
        for (int i = 0; i < short_count_; i++) {
            if (now_ms - short_ms_[i] < UNSTABLE_WINDOW_MS) short_ms_[keep++] = short_ms_[i];
        }
        short_count_ = keep;
    }

    bool ever_up_ = false;
    bool down_ = false;
    bool down_local_ = false;
    bool lost_logged_ = false;
    int64_t down_since_ms_ = 0;
    uint16_t reason_ = 0;
    int64_t short_ms_[UNSTABLE_COUNT] = {};
    int short_count_ = 0;
    int64_t quiet_until_ms_ = 0;  // after an UNSTABLE report, 0 when none
    uint32_t quiet_drops_ = 0;    // short drops held during the quiet window
    WifiDropEvent pending_[MAX_PENDING] = {};
    int pending_count_ = 0;
    WifiDropStats stats_ = {};
};
