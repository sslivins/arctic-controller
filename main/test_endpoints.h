/*
 * Test Instrumentation Endpoints
 * 
 * Provides REST API endpoints for automated UI testing:
 * - GET  /api/test/ui-state  — Walk the LVGL widget tree and return visible labels/buttons
 * - POST /api/test/click     — Find a widget by label text and click it
 *
 * These endpoints are compiled only when CONFIG_TEST_ENDPOINTS is enabled.
 * They require the LVGL display mutex (bsp_display_lock) for thread safety.
 */

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_TEST_ENDPOINTS

#include <esp_http_server.h>

/**
 * Register all test instrumentation endpoints on the given HTTP server.
 */
void test_endpoints_register(httpd_handle_t server);

#endif // CONFIG_TEST_ENDPOINTS
