#include "ota_commit.h"

#include "ota_manager.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char* TAG = "ota_commit";

#define NVS_NAMESPACE       "ota_commit"
#define NVS_KEY_COMMISSIONED "commissioned"

// How long an as-yet-uncommissioned device waits for a network before it
// commits on UI alone. A device that has never had an IP is being set up from
// the touchscreen (there is no SoftAP provisioning path on this hardware), so
// it may legitimately never connect. The grace only exists so that a device
// which *is* about to connect records itself commissioned first, rather than
// taking the weaker UI-only path by racing DHCP.
static const int64_t UNCOMMISSIONED_GRACE_US = 120LL * 1000000LL;

// Nag cadence for an image that is stuck uncommitted, so the reason is visible
// in the serial log without flooding it.
static const int64_t NAG_INTERVAL_US = 60LL * 1000000LL;

static bool s_ui_ready = false;
static bool s_network_seen = false;
static bool s_commissioned = false;
static bool s_committed = false;
static bool s_logged_pending = false;
static int64_t s_first_eval_us = 0;
static int64_t s_next_nag_us = 0;
static const char* s_status = "not_pending";

static void persist_commissioned(void)
{
    if (s_commissioned) {
        return;
    }
    s_commissioned = true;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        // Non-fatal: the latch is an optimisation for the *next* boot. Losing
        // it only means a future uncommissioned-looking boot takes the weaker
        // UI-only path, which is what the old firmware did unconditionally.
        ESP_LOGW(TAG, "Could not open NVS to record commissioning: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, NVS_KEY_COMMISSIONED, 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist commissioning latch: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Device commissioned — future OTAs must prove reachability to commit");
    }
    nvs_close(nvs);
}

void ota_commit_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nvs, NVS_KEY_COMMISSIONED, &v) == ESP_OK) {
            s_commissioned = (v != 0);
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Commit criteria armed (commissioned=%s)", s_commissioned ? "yes" : "no");
}

void ota_commit_note_ui_ready(void)
{
    s_ui_ready = true;
}

bool ota_commit_is_commissioned(void)
{
    return s_commissioned;
}

const char* ota_commit_status(void)
{
    return s_status;
}

// Reachability means an association plus an address we could actually be
// contacted on. Deliberately local: no server round-trip, so a device whose
// AP is up but whose internet is down still commits.
static bool network_is_up(void)
{
    if (wifi_mgr_get_state() != WIFI_MGR_STATE_CONNECTED) {
        return false;
    }
    char ip[16] = {0};
    if (!wifi_mgr_get_ip_addr(ip, sizeof(ip))) {
        return false;
    }
    return ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

static void commit(const char* why)
{
    ESP_LOGI(TAG, "Commit criteria met (%s) — marking firmware valid", why);
    ota_mgr_mark_valid();
    s_committed = true;
    s_status = "committed";
}

void ota_commit_eval(void)
{
    if (s_committed) {
        return;
    }

    // Latch reachability regardless of pending state, so the commissioned flag
    // is recorded on ordinary boots too and is already present the first time
    // an OTA lands.
    if (!s_network_seen && network_is_up()) {
        s_network_seen = true;
        persist_commissioned();
    }

    if (!ota_mgr_is_pending_verify()) {
        s_status = "not_pending";
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (s_first_eval_us == 0) {
        s_first_eval_us = now;
        s_next_nag_us = now + NAG_INTERVAL_US;
    }
    if (!s_logged_pending) {
        s_logged_pending = true;
        ESP_LOGW(TAG, "OTA_PENDING_VERIFY firmware is unverified; commit requires %s",
                 s_commissioned ? "UI + network reachability" : "UI (device not yet commissioned)");
    }

    if (!s_ui_ready) {
        s_status = "waiting_for_ui";
        return;
    }

    if (s_network_seen) {
        commit("ui+network");
        return;
    }

    if (!s_commissioned) {
        // Never been on a network in its life. Give a short grace for DHCP so
        // a device that can connect gets the strong criterion, then accept UI
        // alone rather than stranding a genuinely standalone unit.
        if (now - s_first_eval_us < UNCOMMISSIONED_GRACE_US) {
            s_status = "waiting_for_network_grace";
            return;
        }
        commit("ui-only:never-commissioned");
        return;
    }

    // Commissioned but not reachable on this boot. Stay pending — deliberately
    // forever. No forced reboot: if the network returns in an hour we commit
    // then, and if the device reboots first the bootloader rolls this image
    // back, which is exactly the desired outcome for an image that broke
    // networking.
    s_status = "waiting_for_network";
    if (now >= s_next_nag_us) {
        s_next_nag_us = now + NAG_INTERVAL_US;
        ESP_LOGW(TAG,
                 "OTA_COMMIT_BLOCKED no network since boot (%lld s); firmware stays unverified "
                 "and will roll back if the device reboots",
                 (long long)((now - s_first_eval_us) / 1000000LL));
    }
}
