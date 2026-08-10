/*
 * Arctic Heat Pump Controller
 * Home Assistant production WebSocket push transport
 */
#pragma once

#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ha_websocket_start(httpd_handle_t server);
void ha_websocket_stop(void);

esp_err_t ha_websocket_pre_handshake(httpd_req_t* req);
esp_err_t ha_websocket_handler(httpd_req_t* req);

#ifdef __cplusplus
}
#endif
