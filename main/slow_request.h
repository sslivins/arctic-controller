/*
 * Arctic Heat Pump Controller
 * Worker task for slow HTTP handlers.
 *
 * Every esp_http_server instance runs on a single thread, so a handler that
 * takes seconds (a screenshot is ~10 s of PNG streaming) stalls every other
 * client of that port -- including the controller's own /api/health
 * self-probe, which then restarts the servers. Handlers registered here hand
 * their request to one worker task (httpd_req_async_handler_begin) and return
 * at once, so the server thread keeps serving.
 */
#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

typedef esp_err_t (*slow_request_handler_t)(httpd_req_t* req);

/**
 * @brief Run @p handler for @p req on the slow-request worker.
 *
 * Call from a URI handler that is not yet on the worker:
 *
 *     if (!slow_request_on_worker()) return slow_request_defer(req, my_handler);
 *
 * The handler runs later with an async copy of the request. If the worker is
 * busy with too many queued requests (or the servers are stopping) the client
 * gets 503 instead. A non-ESP_OK return from @p handler closes the connection,
 * as it would on the server thread.
 */
esp_err_t slow_request_defer(httpd_req_t* req, slow_request_handler_t handler);

/** @brief True when called from the slow-request worker. */
bool slow_request_on_worker(void);

/**
 * @brief True while the servers are being stopped. Long-running handlers
 *        check it between chunks and give up, so the stop is not held up.
 */
bool slow_request_aborting(void);

/**
 * @brief Take the LVGL display lock from the worker, giving up if the
 *        servers start stopping meanwhile. Pair with bsp_display_unlock().
 */
bool slow_request_display_lock(void);

/**
 * @brief Finish or abandon every queued and running request, and refuse new
 *        ones until slow_request_resume().
 *
 * Must be called before httpd_stop() on a server that may have handed
 * requests to the worker: the async copies point into the server's session
 * table. Blocks until the worker is idle (bounded; the request in flight has
 * its socket shut down so a stalled client cannot hold the stop).
 */
void slow_request_drain(void);

/** @brief Accept requests again once the servers are stopped. */
void slow_request_resume(void);
