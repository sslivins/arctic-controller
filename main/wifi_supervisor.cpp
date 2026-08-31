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
#include "system_restart.h"
#include "log_persist.h"
#include "event_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <string.h>

#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

static const char* TAG = "wifi_super";


// Cadence / thresholds. Chosen so the supervisor is invisible during normal
// operation and gentle during an outage (the local touchscreen keeps working
// without WiFi), escalating to a reboot only as a genuine last resort.
static const int CHECK_INTERVAL_MS  = 15000;    // poll cadence
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
static const uint32_t RECOVER_SELF   = 0;    // came back on its own (keepalive)
static const uint32_t RECOVER_BOUNCE = 1;    // a WiFi link bounce restored it
static const uint32_t RECOVER_REBOOT = 2;    // giving up -> rebooting

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
    int disconnected_ms = 0;
    int since_attempt_ms = RECONNECT_EVERY_MS;  // allow a first attempt after grace

    // Reachability-supervision state. unreachable_since_us anchors the outage to
    // the monotonic clock (so time spent disconnected mid-bounce still counts).
    int64_t unreachable_since_us = 0;
    bool    unreachable_logged   = false;  // recorded EVENT_NETWORK_UNREACHABLE yet
    int     bounces              = 0;      // WiFi bounces performed this outage
    bool    ever_reachable       = false;  // gate: only supervise once first seen up

    // Let the initial boot-time connection flow run before supervising.
    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));

    for (;;) {
        wifi_mgr_state_t state = wifi_mgr_get_state();
        const bool have_creds = wifi_mgr_has_saved_credentials();

        if (!have_creds) {
            // Nothing to supervise. Stay fully passive and reset all state.
            disconnected_ms = 0;
            since_attempt_ms = RECONNECT_EVERY_MS;
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
            disconnected_ms += CHECK_INTERVAL_MS;
            since_attempt_ms += CHECK_INTERVAL_MS;

            // While an association attempt is already in flight (including the
            // app's own boot-time or user-initiated connect, or a recovery
            // bounce's reconnect), do NOT issue a competing reconnect. Only
            // re-issue once the link is genuinely idle (DISCONNECTED / ERROR)
            // past the grace + backoff windows.
            bool connecting = (state == WIFI_MGR_STATE_CONNECTING);
            if (!connecting && disconnected_ms >= GRACE_MS &&
                since_attempt_ms >= RECONNECT_EVERY_MS) {
                char ssid[33] = {0};
                char password[65] = {0};
                if (wifi_mgr_load_credentials(ssid, sizeof(ssid),
                                              password, sizeof(password))) {
                    ESP_LOGW(TAG, "WiFi down %ds - reconnecting to '%s'",
                             disconnected_ms / 1000, ssid);
                    wifi_mgr_connect(ssid, password, nullptr);
                }
                since_attempt_ms = 0;
            }

            if (disconnected_ms >= REBOOT_AFTER_MS) {
                ESP_LOGE(TAG, "WiFi down %ds and unrecoverable in place - restarting "
                         "to reinitialize the network stack", disconnected_ms / 1000);
                log_persist_flush_now(LOG_PERSIST_REASON_MANUAL);
                vTaskDelay(pdMS_TO_TICKS(500));  // flush logs
                system_safe_restart();
            }

            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

        // ---- Associated: reset association bookkeeping ----
        disconnected_ms = 0;
        since_attempt_ms = RECONNECT_EVERY_MS;

        // ---- Reachability (network-stack wedge) supervision ----
        char ip[16] = {0};
        const bool have_ip = wifi_mgr_get_ip_addr(ip, sizeof(ip)) &&
                             ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
        if (!have_ip) {
            // Associated but no IP yet (DHCP in progress) - not a wedge.
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

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
                }
            }
            unreachable_since_us = 0;
            unreachable_logged = false;
            bounces = 0;
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

        // Unreachable while associated and holding an IP.
        if (!ever_reachable) {
            // Never came up since boot: servers/certs/DHCP may still be settling.
            // Recovery would be premature, so just wait.
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (unreachable_since_us == 0) {
            unreachable_since_us = now;
        }
        const int down_ms = (int)((now - unreachable_since_us) / 1000);
        ESP_LOGW(TAG, "associated (ip=%s) but gateway unreachable for %ds "
                 "(bounces so far %d)", ip, down_ms / 1000, bounces);

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

        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

void wifi_supervisor_start(void) {
    // 5 KB stack: the recovery path calls wifi_mgr_connect()/disconnect() and
    // event_log_record() inline; the ICMP ping runs in its own esp_ping task.
    xTaskCreate(wifi_supervisor_task, "wifi_super", 5120, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "WiFi health supervisor started");
}
