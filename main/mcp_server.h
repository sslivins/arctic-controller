/*
 * Arctic Heat Pump Controller
 * Embedded MCP (Model Context Protocol) Server
 *
 * Implements MCP 2025-03-26 over Streamable HTTP transport.
 * Allows LLM clients to discover and interact with the heat pump
 * controller through structured tools and resources.
 *
 * Endpoint: POST /mcp  (JSON-RPC 2.0)
 *           GET  /mcp  (SSE stream, returns 405 — not supported)
 *           DELETE /mcp (session termination)
 */
#pragma once

#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register MCP server HTTP handlers
 *
 * Registers POST /mcp and GET /mcp endpoints on the existing HTTP server.
 * Must be called after httpd_start().
 *
 * @param server  The running HTTP server handle
 * @return true on success
 */
bool mcp_server_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
