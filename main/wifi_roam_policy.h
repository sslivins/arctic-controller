/*
 * Arctic Heat Pump Controller
 * WiFi roaming policy - when to look for a better access point and which one
 * to move to. Pure logic over a caller-supplied clock so it can be host-tested.
 *
 * The ESP32-C6's stock ESP-Hosted firmware has no 802.11k/v roaming, so the
 * controller roams itself. Two things trigger a scan of the network's access
 * points: a link that stays weak for a minute, and a one-off check two minutes
 * after each new connection that isn't already good (the C6 doesn't reliably
 * join the strongest AP, so a fresh connection can sit on a mediocre one that
 * never counts as weak).
 * We move only to an access point that is clearly stronger. Each roam costs a
 * few seconds of reconnect, so the policy is deliberately conservative and
 * rate-limited.
 */
#pragma once

#include <stdint.h>
#include <string.h>

struct WifiRoamCandidate {
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
};

class WifiRoamPolicy {
public:
    static constexpr int WEAK_RSSI = -70;          // dBm at or below = weak
    static constexpr int GOOD_RSSI = -60;          // above this, a fresh connection is left alone
    static constexpr int64_t WEAK_FOR_MS = 60000;  // weak this long before scanning
    static constexpr int64_t SETTLE_MS = 120000;   // after (re)connecting
    static constexpr int64_t RESCAN_MS = 900000;   // between scans / after a roam
    static constexpr int MIN_GAIN_DB = 10;         // target must beat current by this

    // RSSI sample (0 = unknown) while connected or not. True when a roam
    // scan should run now.
    bool sample(int64_t now, bool connected, int rssi) {
        if (!connected) {
            connected_since_ = -1;
            weak_since_ = -1;
            connect_checked_ = false;
            return false;
        }
        if (connected_since_ < 0) connected_since_ = now;
        if (now - connected_since_ < SETTLE_MS) {
            if (rssi == 0 || rssi > WEAK_RSSI) weak_since_ = -1;
            else if (weak_since_ < 0) weak_since_ = now;
            return false;
        }
        if (!connect_checked_) {
            // A good link isn't worth a reconnect, however much better
            // another AP is.
            connect_checked_ = true;
            if (rssi != 0 && rssi <= GOOD_RSSI) return true;
        }
        if (rssi == 0 || rssi > WEAK_RSSI) {
            weak_since_ = -1;
            return false;
        }
        if (weak_since_ < 0) weak_since_ = now;
        if (now - weak_since_ < WEAK_FOR_MS) return false;
        if (last_scan_ >= 0 && now - last_scan_ < RESCAN_MS) return false;
        return true;
    }

    // Records a scan and returns the index of the access point to roam to,
    // or -1 to stay. `any` (test only) takes the strongest other AP even if
    // it is not better.
    int pick(int64_t now, const uint8_t current[6], int current_rssi,
             const WifiRoamCandidate* c, int n, bool any = false) {
        last_scan_ = now;
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (memcmp(c[i].bssid, current, 6) == 0) continue;
            if (!any && c[i].rssi < current_rssi + MIN_GAIN_DB) continue;
            if (best < 0 || c[i].rssi > c[best].rssi) best = i;
        }
        return best;
    }

    // A roam was attempted (successful or not): settle before judging again.
    void on_roam(int64_t now) {
        connected_since_ = now;
        weak_since_ = -1;
        last_scan_ = now;
        connect_checked_ = true;
    }

private:
    int64_t connected_since_ = -1;
    int64_t weak_since_ = -1;
    int64_t last_scan_ = -1;
    bool connect_checked_ = false;
};
