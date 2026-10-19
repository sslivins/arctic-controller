/*
 * Arctic Heat Pump Controller
 * WiFi health supervisor
 *
 * A lightweight background task that watches the WiFi connection and recovers
 * from prolonged loss without human intervention. A drop the controller did
 * not ask for (AP kick, AP restart, signal loss) gets one reconnect a couple of
 * seconds later. If the device has saved credentials but stays disconnected
 * past a grace period it re-issues a connect every 30 s; if it still cannot
 * re-associate after repeated attempts it reboots
 * (esp_restart), which re-initializes the ESP-Hosted C6 link and clears wedged
 * states. It stays completely passive while the connection is healthy or while
 * no credentials are configured, so it is a no-op during normal operation.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the WiFi health supervisor task. Call once after the WiFi
 *        initialization task has been started. Safe to call before WiFi has
 *        finished connecting — the supervisor waits for a grace period before
 *        acting and ignores the initial CONNECTING phase.
 */
void wifi_supervisor_start(void);

/**
 * @brief Wake the supervisor after the link dropped, so an unplanned drop
 *        gets its quick reconnect without waiting for the next check. Safe
 *        from the WiFi event handler; a no-op before the supervisor starts.
 */
void wifi_supervisor_notify_disconnect(void);

#ifdef __cplusplus
}
#endif
