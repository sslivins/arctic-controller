/*
 * Arctic Heat Pump Controller
 * WiFi health supervisor - implementation
 *
 * Supervises two independent failure modes:
 *
 *   1. Association loss  - the C6 radio link drops. Detected via
 *      wifi_mgr_get_state(); recovered by in-place reconnect, with a 15-minute
 *      reboot backstop.
 *
 *   2. Network-stack wedge - the device stays associated (the C6 link is fine
 *      and the touchscreen keeps working) but the on-board TCP/IP stack stops
 *      serving connections. This is the "everything alive except the network"
 *      failure that no existing watchdog catches: the task WDT sees healthy,
 *      well-fed tasks and the association check sees a connected radio, so the
 *      device sits unreachable until someone power-cycles it by hand.
 *
 *      Root cause (now fixed at source in api_server.cpp by enabling TCP
 *      keepalive on every httpd server): dead ESTABLISHED sockets from clients
 *      that vanished mid-request were never reaped, leaking heap + PCBs until
 *      the single-threaded stack could no longer accept() or answer ICMP.
 *      Keepalive makes those sockets self-reap in ~25s, so the wedge should no
 *      longer become permanent. This supervisor is the belt-and-suspenders net
 *      in case a wedge still occurs.
 *
 *      Detection MUST use an EXTERNAL target: CONFIG_LWIP_NETIF_LOOPBACK=y means
 *      any packet addressed to 127.0.0.1 OR to our own STA IP is hairpinned
 *      internally and never crosses the C6 radio, so a loopback/self probe
 *      stays "reachable" even while the device is unreachable on the LAN. We
 *      therefore ICMP-ping the DEFAULT GATEWAY. During the wedge the ping task
 *      still runs (CPU is healthy) but gets no reply, correctly reporting
 *      unreachable. If no gateway is known / a session can't be created we
 *      report reachable so we never false-trigger.
 *
 *      Recovery is a WiFi link bounce (disconnect + immediate reconnect with
 *      saved credentials). Taking the netif down aborts every PCB, instantly
 *      freeing the leaked heap/sockets, then we cleanly re-associate. This was
 *      proven on a live-wedged device (heap jumped 35K->140K, tcp active->0).
 *      If repeated bounces don't restore reachability we reboot as a last
 *      resort. Both the detected outage and the recovery are written to the
 *      event log so the fleet operator can see it happened, and a
 *      persistent-log snapshot is flushed to flash before each recovery action.
 */
#include "wifi_supervisor.h"
#include "wifi_manager.h"
#include "api_server.h"
#include "system_restart.h"
#include "log_persist.h"
#include "event_log.h"
#include "net_diag.h"
#include "wifi_drops.h"
#include "wifi_roam.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <string.h>
#include <atomic>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

static const char* TAG = "wifi_super";


// Cadence / thresholds. Chosen so the supervisor is invisible during normal
// operation and gentle during an outage (the local touchscreen keeps working
// without WiFi), escalating to a reboot only as a genuine last resort.
static const int CHECK_INTERVAL_MS  = 15000;    // poll cadence
static const int FAST_RECONNECT_MS  = 2000;     // one quick retry after an unplanned drop
static const int GRACE_MS           = 60000;    // tolerate assoc loss before acting
static const int RECONNECT_EVERY_MS = 30000;    // in-place reconnect cadence
static const int REBOOT_AFTER_MS    = 900000;   // 15 min continuously down -> reboot

// Reachability (network-stack wedge) supervision.
//   - NOTABLE_MS: once the gateway has been unreachable this long we record an
//     EVENT_NETWORK_UNREACHABLE (even if it later self-heals via keepalive), so
//     the outage is always visible in the event log. Short blips below this are
//     ignored as noise.
//   - REACH_GRACE_MS: unreachable this long before the first WiFi bounce.
//   - BOUNCE_STEP_MS: spacing between successive bounces.
//   - MAX_BOUNCES: after this many bounces fail to restore reachability, reboot.
static const int NOTABLE_MS      = 30000;    // 30s -> log the outage
static const int REACH_GRACE_MS  = 90000;    // 90s -> first WiFi bounce
static const int BOUNCE_STEP_MS  = 45000;    // gap between bounces / before reboot
static const int MAX_BOUNCES     = 2;

// EVENT_NETWORK_RECOVERED payload (recovery method).
static const uint32_t RECOVER_SELF   = EVENT_NETWORK_RECOVERED_SELF;
static const uint32_t RECOVER_BOUNCE = EVENT_NETWORK_RECOVERED_WIFI_BOUNCE;
static const uint32_t RECOVER_REBOOT = EVENT_NETWORK_RECOVERED_REBOOT;

// Web/API server safety net. The servers are started once and kept running
// across WiFi drops, but if they are ever found down while we hold an IP the
// device is pingable yet unmanageable (the field failure that needed a power
// cycle). Restart them in place every check; if that keeps failing, reboot.
//   - API_BOOT_GRACE_MS: never judge before this uptime unless the servers
//     have already been healthy once (the boot-time start may be in flight).
//   - API_DOWN_REBOOT_MS: continuously down this long -> reboot.
//   - API_MAX_REBOOTS: consecutive safety-net reboots allowed before we stop
//     rebooting (and just keep retrying in place) so a fault that survives a
//     reboot cannot boot-loop the controller. Only back-to-back safety-net
//     reboots count: any other reset (power-on, OTA, crash, user restart) or
//     API_HEALTHY_RESET_MS of healthy running starts the count again.
static const int64_t  API_BOOT_GRACE_MS      = 120000;
static const int      API_RESTART_TIMEOUT_MS = 20000;
static const int      API_DOWN_REBOOT_MS     = 90000;
static const int64_t  API_HEALTHY_RESET_MS   = 600000;
static const uint32_t API_MAX_REBOOTS        = 2;
static const uint32_t API_REBOOT_MAGIC       = 0x41504952;  // "APIR"

// Survive software resets (not power loss) so the reboot cap spans reboots.
RTC_NOINIT_ATTR static uint32_t s_api_reboot_magic;
RTC_NOINIT_ATTR static uint32_t s_api_reboot_count;
RTC_NOINIT_ATTR static uint32_t s_api_reboot_ours;  // magic if we caused the last reset

static uint32_t web_server_cause(api_start_error_t err) {
    switch (err) {
        case API_START_ERR_NO_MEMORY:   return EVENT_WEB_SERVER_CAUSE_NO_MEMORY;
        case API_START_ERR_NO_IDENTITY: return EVENT_WEB_SERVER_CAUSE_NO_IDENTITY;
        case API_START_ERR_FAILED:      return EVENT_WEB_SERVER_CAUSE_START_FAILED;
        case API_START_ERR_INJECTED:    return EVENT_WEB_SERVER_CAUSE_TEST;
        default:                        return EVENT_WEB_SERVER_CAUSE_UNKNOWN;
    }
}

struct ApiWatch {
    int64_t down_since_us = 0;
    int64_t healthy_since_us = 0;
    bool    ever_healthy = false;
    bool    down_logged = false;
    bool    budget_note_logged = false;
    int     attempts = 0;
};

// One safety-net step; called each check while associated with an IP.
static void supervise_api_server(ApiWatch& w) {
    if (api_server_is_shut_down()) {
        return;  // OTA flashing: a reboot follows
    }
    const int64_t now = esp_timer_get_time();

    if (api_server_is_healthy()) {
        if (w.down_since_us != 0) {
            // Came back without our restart (e.g. a GOT_IP start finished).
            event_log_record(EVENT_WEB_SERVER_RECOVERED,
                             EVENT_WEB_SERVER_RECOVERED_RESTARTED);
            ESP_LOGW(TAG, "web/API servers healthy again");
            w.down_since_us = 0;
        }
        w.ever_healthy = true;
        if (w.healthy_since_us == 0) w.healthy_since_us = now;
        if (s_api_reboot_count != 0 &&
            (now - w.healthy_since_us) / 1000 >= API_HEALTHY_RESET_MS) {
            s_api_reboot_count = 0;
        }
        return;
    }

    w.healthy_since_us = 0;
    if (!w.ever_healthy && now / 1000 < API_BOOT_GRACE_MS) {
        return;
    }
    if (w.down_since_us == 0) {
        w.down_since_us = now;
        w.down_logged = false;
        w.budget_note_logged = false;
        w.attempts = 0;
    }

    const api_start_error_t prior = api_server_last_start_error();
    w.attempts++;
    ESP_LOGE(TAG, "web/API servers down while online - restart attempt %d "
             "(internal free=%u largest=%u)", w.attempts,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const bool ok = wifi_mgr_api_ensure_running(API_RESTART_TIMEOUT_MS);

    if (!w.down_logged) {
        const api_start_error_t cause = ok ? prior : api_server_last_start_error();
        event_log_record(EVENT_WEB_SERVER_DOWN, web_server_cause(cause));
        log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
        w.down_logged = true;
    }

    if (ok) {
        ESP_LOGW(TAG, "web/API servers restarted after %d attempt(s)", w.attempts);
        event_log_record(EVENT_WEB_SERVER_RECOVERED,
                         EVENT_WEB_SERVER_RECOVERED_RESTARTED);
        w.down_since_us = 0;
        w.ever_healthy = true;
        return;
    }

    const int down_ms = (int)((esp_timer_get_time() - w.down_since_us) / 1000);
    if (down_ms < API_DOWN_REBOOT_MS) {
        return;
    }
    if (s_api_reboot_magic != API_REBOOT_MAGIC) {
        s_api_reboot_magic = API_REBOOT_MAGIC;
        s_api_reboot_count = 0;
    }
    if (s_api_reboot_count >= API_MAX_REBOOTS) {
        if (!w.budget_note_logged) {
            ESP_LOGE(TAG, "web/API servers still down after %d reboots - not "
                     "rebooting again, retrying in place", (int)API_MAX_REBOOTS);
            w.budget_note_logged = true;
        }
        return;
    }
    s_api_reboot_count++;
    s_api_reboot_ours = API_REBOOT_MAGIC;
    ESP_LOGE(TAG, "web/API servers down %ds after %d restart attempts - rebooting "
             "(%u/%u)", down_ms / 1000, w.attempts, (unsigned)s_api_reboot_count,
             (unsigned)API_MAX_REBOOTS);
    event_log_record(EVENT_WEB_SERVER_RECOVERED, EVENT_WEB_SERVER_RECOVERED_REBOOT);
    log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
    vTaskDelay(pdMS_TO_TICKS(500));
    system_safe_restart();
}

static TaskHandle_t s_task = nullptr;

// Sleeps until the next check, or until the link drops.
static void wait_for_next_check(int ms) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms > 0 ? ms : 1));
}

void wifi_supervisor_notify_disconnect(void) {
    TaskHandle_t t = s_task;
    if (t != nullptr) xTaskNotifyGive(t);
}

// Set when the link comes up, cleared by the supervisor. Without it a drop
// shortly after a reconnect the supervisor slept through looks like the same
// outage: no quick retry, and the grace period counted from the earlier drop.
static std::atomic<bool> s_link_up_seen{false};

void wifi_supervisor_notify_connected(void) {
    s_link_up_seen.store(true);
    TaskHandle_t t = s_task;
    if (t != nullptr) xTaskNotifyGive(t);
}

static SemaphoreHandle_t s_ping_done = nullptr;

static void ping_on_end(esp_ping_handle_t hdl, void* args) {
    (void)hdl;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)args;
    if (sem != nullptr) xSemaphoreGive(sem);
}

// Active reachability probe: ICMP-ping the default gateway. A reply proves the
// full radio + TCP/IP external path is alive. During the network-stack wedge
// the ping gets NO reply (recv=0) even though association is up, which is the
// signal we act on. If we can't determine a gateway, or can't create/run a ping
// session, we deliberately report reachable so the supervisor never triggers a
// recovery on ambiguous evidence.
static bool probe_gateway_reachable(void) {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) return true;

    esp_netif_ip_info_t ipinfo;
    if (esp_netif_get_ip_info(sta, &ipinfo) != ESP_OK) return true;
    if (ipinfo.gw.addr == 0) return true;   // no gateway known yet

    if (s_ping_done == nullptr) {
        s_ping_done = xSemaphoreCreateBinary();
        if (s_ping_done == nullptr) return true;
    }

    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ipinfo.gw.addr;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr     = target;
    cfg.count           = 3;
    cfg.interval_ms     = 250;
    cfg.timeout_ms      = 1000;
    cfg.task_stack_size = 3072;
    cfg.task_prio       = 3;

    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cb_args    = s_ping_done;
    cbs.on_ping_end = ping_on_end;

    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || hdl == nullptr) {
        return true;
    }

    // Drain any stale completion signal, then run the session to completion.
    xSemaphoreTake(s_ping_done, 0);
    bool reachable = true;
    if (esp_ping_start(hdl) != ESP_OK) {
        esp_ping_delete_session(hdl);
        return true;
    }

    // Worst case ~3*250ms + one 1000ms timeout; wait generously.
    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(5000)) == pdTRUE) {
        uint32_t received = 0;
        esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
        reachable = (received > 0);
    } else {
        // The ping task itself never finished within the window - treat the
        // stack as unreachable.
        esp_ping_stop(hdl);
        reachable = false;
    }
    esp_ping_delete_session(hdl);
    return reachable;
}

static void wifi_supervisor_task(void* param) {
    (void)param;
    int64_t down_since_us = 0;    // association lost at, 0 while associated
    int64_t last_attempt_us = 0;  // our last reconnect this outage, 0 if none
    bool    fast_tried = false;   // the quick retry has been used this outage

    // Reachability-supervision state. unreachable_since_us anchors the outage to
    // the monotonic clock (so time spent disconnected mid-bounce still counts).
    int64_t unreachable_since_us = 0;
    bool    unreachable_logged   = false;  // recorded EVENT_NETWORK_UNREACHABLE yet
    int     bounces              = 0;      // WiFi bounces performed this outage
    bool    ever_reachable       = false;  // gate: only supervise once first seen up
    ApiWatch api_watch;

    // The reboot cap lives in RTC no-init RAM: garbage after power loss, and
    // only meaningful if the reset we are booting from was our own.
    if (s_api_reboot_magic != API_REBOOT_MAGIC ||
        s_api_reboot_ours != API_REBOOT_MAGIC ||
        esp_reset_reason() != ESP_RST_SW) {
        s_api_reboot_magic = API_REBOOT_MAGIC;
        s_api_reboot_count = 0;
    }
    s_api_reboot_ours = 0;

    // Let the initial boot-time connection flow run before supervising.
    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));

    for (;;) {
        wifi_drops_poll();
        wifi_mgr_state_t state = wifi_mgr_get_state();
        const bool have_creds = wifi_mgr_has_saved_credentials();

        if (!have_creds) {
            // Nothing to supervise. Stay fully passive and reset all state.
            down_since_us = 0;
            last_attempt_us = 0;
            fast_tried = false;
            unreachable_since_us = 0;
            unreachable_logged = false;
            bounces = 0;
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

        if (state != WIFI_MGR_STATE_CONNECTED) {
            // ---- Association supervision ----
            // Not connected, saved credentials present. Track total downtime for
            // the reboot backstop regardless of sub-state. Deliberately do NOT
            // clear the reachability outage clock here: if we are here because a
            // recovery bounce dropped the link, the outage is ongoing and must
            // keep counting toward escalation.
            const int64_t now = esp_timer_get_time();
            if (s_link_up_seen.exchange(false)) {
                down_since_us = 0;
                last_attempt_us = 0;
                fast_tried = false;
            }
            if (down_since_us == 0) down_since_us = now;
            const int down_ms = (int)((now - down_since_us) / 1000);

            // While an association attempt is already in flight (including the
            // app's own boot-time or user-initiated connect, or a recovery
            // bounce's reconnect), do NOT issue a competing reconnect. Only
            // re-issue once the link is genuinely idle (DISCONNECTED / ERROR).
            // A drop we did not cause (AP kick or restart, signal loss) gets one
            // quick retry; after that, and for everything else, wait out the
            // grace period and then retry on the usual cadence. Our own
            // disconnects (roams, bounces, network changes) reconnect themselves.
            const bool connecting = (state == WIFI_MGR_STATE_CONNECTING);
            const bool fast_due = !fast_tried && wifi_drops_stats().down &&
                                  !wifi_roam_in_progress();
            const bool slow_due = down_ms >= GRACE_MS &&
                (last_attempt_us == 0 ||
                 (now - last_attempt_us) / 1000 >= RECONNECT_EVERY_MS);
            if (!connecting && ((fast_due && down_ms >= FAST_RECONNECT_MS) || slow_due)) {
                char ssid[33] = {0};
                char password[65] = {0};
                if (wifi_mgr_load_credentials(ssid, sizeof(ssid),
                                              password, sizeof(password))) {
                    ESP_LOGW(TAG, "WiFi down %ds - reconnecting to '%s'",
                             down_ms / 1000, ssid);
                    wifi_mgr_connect(ssid, password, nullptr);
                }
                memset(password, 0, sizeof(password));
                last_attempt_us = now;
                fast_tried = true;
            }

            if (down_ms >= REBOOT_AFTER_MS) {
                ESP_LOGE(TAG, "WiFi down %ds and unrecoverable in place - restarting "
                         "to reinitialize the network stack", down_ms / 1000);
                log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
                vTaskDelay(pdMS_TO_TICKS(500));  // flush logs
                system_safe_restart();
            }

            int wait_ms = CHECK_INTERVAL_MS;
            if (!fast_tried && fast_due && !connecting) {
                wait_ms = FAST_RECONNECT_MS - down_ms;
            }
            wait_for_next_check(wait_ms);
            continue;
        }

        // ---- Associated: reset association bookkeeping ----
        s_link_up_seen.store(false);
        down_since_us = 0;
        last_attempt_us = 0;
        fast_tried = false;

        // ---- Reachability (network-stack wedge) supervision ----
        char ip[16] = {0};
        const bool have_ip = wifi_mgr_get_ip_addr(ip, sizeof(ip)) &&
                             ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
        if (!have_ip) {
            // Associated but no IP yet (DHCP in progress) - not a wedge.
            wait_for_next_check(CHECK_INTERVAL_MS);
            continue;
        }

        // ---- Web/API server safety net ----
        supervise_api_server(api_watch);

        const bool reachable = probe_gateway_reachable();

        if (reachable) {
            if (!ever_reachable) {
                ever_reachable = true;
                ESP_LOGI(TAG, "network reachable (ip=%s, gateway pingable) - "
                         "wedge supervision armed", ip);
            }
            if (unreachable_since_us != 0) {
                const int down_ms =
                    (int)((esp_timer_get_time() - unreachable_since_us) / 1000);
                if (unreachable_logged) {
                    const uint32_t method = (bounces > 0) ? RECOVER_BOUNCE
                                                          : RECOVER_SELF;
                    event_log_record(EVENT_NETWORK_RECOVERED, method);
                    ESP_LOGW(TAG, "gateway reachable again after %ds "
                             "(recovered via %s)", down_ms / 1000,
                             method == RECOVER_BOUNCE ? "wifi-bounce"
                                                      : "self-heal/keepalive");
                } else {
                    // Too short for the event log, but still worth a line so
                    // the outage's end is visible next to its start.
                    char wifi[112];
                    net_diag_format_wifi(wifi, sizeof(wifi));
                    ESP_LOGW(TAG, "gateway reachable again after %ds "
                             "(brief outage, not recorded) | %s",
                             down_ms / 1000, wifi);
                }
            }
            unreachable_since_us = 0;
            unreachable_logged = false;
            bounces = 0;
            // Only from a healthy link, so roaming never overlaps recovery.
            wifi_roam_poll();
            wait_for_next_check(CHECK_INTERVAL_MS);
            continue;
        }

        // Unreachable while associated and holding an IP.
        if (!ever_reachable) {
            // Never came up since boot: servers/certs/DHCP may still be settling.
            // Recovery would be premature, so just wait.
            wait_for_next_check(CHECK_INTERVAL_MS);
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (unreachable_since_us == 0) {
            unreachable_since_us = now;
        }
        const int down_ms = (int)((now - unreachable_since_us) / 1000);
        char wifi[112];
        net_diag_format_wifi(wifi, sizeof(wifi));
        ESP_LOGW(TAG, "associated (ip=%s) but gateway unreachable for %ds "
                 "(bounces so far %d) | %s", ip, down_ms / 1000, bounces, wifi);

        // Record the outage once it is clearly not a transient blip, even if it
        // self-heals before we bounce (that itself confirms keepalive working).
        if (!unreachable_logged && down_ms >= NOTABLE_MS) {
            event_log_record(EVENT_NETWORK_UNREACHABLE, 0);
            log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
            unreachable_logged = true;
            ESP_LOGE(TAG, "network-stack wedge suspected: gateway unreachable "
                     "%ds while associated (ip=%s)", down_ms / 1000, ip);
        }

        if (bounces < MAX_BOUNCES &&
            down_ms >= REACH_GRACE_MS + bounces * BOUNCE_STEP_MS) {
            // Recovery: bounce the WiFi link. Taking the netif down aborts every
            // PCB, freeing the leaked heap/sockets, then we re-associate cleanly.
            ESP_LOGE(TAG, "recovery: bouncing WiFi link (attempt %d/%d) to clear "
                     "the network-stack wedge", bounces + 1, MAX_BOUNCES);
            log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
            char ssid[33] = {0};
            char password[65] = {0};
            wifi_mgr_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (wifi_mgr_load_credentials(ssid, sizeof(ssid),
                                          password, sizeof(password))) {
                wifi_mgr_connect(ssid, password, nullptr);
            }
            bounces++;
        } else if (bounces >= MAX_BOUNCES &&
                   down_ms >= REACH_GRACE_MS + MAX_BOUNCES * BOUNCE_STEP_MS) {
            ESP_LOGE(TAG, "recovery: %d WiFi bounces did not restore gateway "
                     "reachability after %ds - rebooting", MAX_BOUNCES,
                     down_ms / 1000);
            event_log_record(EVENT_NETWORK_RECOVERED, RECOVER_REBOOT);
            log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
            vTaskDelay(pdMS_TO_TICKS(500));
            system_safe_restart();
        }

        wait_for_next_check(CHECK_INTERVAL_MS);
    }
}

void wifi_supervisor_start(void) {
    // 5 KB stack: the recovery path calls wifi_mgr_connect()/disconnect() and
    // event_log_record() inline; the ICMP ping runs in its own esp_ping task.
    if (xTaskCreate(wifi_supervisor_task, "wifi_super", 5120, nullptr, 4, &s_task) != pdPASS) {
        // Silent failure disables WiFi wedge recovery entirely, which only
        // shows up much later as an unexplained unreachable device.
        ESP_LOGE(TAG, "Failed to create WiFi supervisor task (largest free internal block=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    ESP_LOGI(TAG, "WiFi health supervisor started");
}
