/*
 * Arctic Heat Pump Controller
 * Stall detector for the web UI/API server thread.
 *
 * The web UI server (443 in HTTPS mode, 80 in HTTP mode) is one thread: a
 * slow TLS handshake or handler blocks every other client, and the self-probe
 * only notices after ~30 s and then restarts the servers, destroying the
 * evidence. This posts a no-op work item to the server every second; if it has
 * not run within a few seconds the thread is stuck, and the log gets a task
 * snapshot (state and CPU use of every task while it is stuck) so the cause
 * can be named.
 */
#pragma once

#include <esp_http_server.h>

/** @brief Start watching @p server (the web UI/API server). */
void httpd_watch_attach(httpd_handle_t server);

/** @brief Stop watching. Call before the watched server is stopped. */
void httpd_watch_detach(void);

/**
 * @brief Mark the watched server as legitimately busy (e.g. receiving a
 *        firmware image), so the stall is not reported. Calls nest.
 */
void httpd_watch_busy_begin(void);
void httpd_watch_busy_end(void);
