# Web UI gzip compression
# The embedded dashboard is stored gzipped in flash. Two rules keep the .gz in
# sync with the source HTML:
#   1. execute_process (configure time) guarantees the .gz exists so the initial
#      EMBED_FILES resolution below succeeds on a clean tree.
#   2. add_custom_command (build time) re-gzips whenever web/index.html or the
#      gzip script changes, so a plain `idf.py build` always re-embeds the latest
#      markup WITHOUT needing an explicit `idf.py reconfigure`.
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz")
set(GZIP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py")

# Find Python once - prefer IDF's Python, then system Python.
if(Python3_EXECUTABLE)
    set(PYTHON_CMD "${Python3_EXECUTABLE}")
else()
    find_program(PYTHON_CMD NAMES python3 python)
endif()
if(NOT PYTHON_CMD)
    message(FATAL_ERROR "Python not found - required for gzip compression")
endif()

# Configure-time: ensure the .gz exists for the initial EMBED_FILES resolution.
if(EXISTS "${WEB_SRC}" AND (NOT EXISTS "${WEB_GZ}" OR "${WEB_SRC}" IS_NEWER_THAN "${WEB_GZ}"))
    message(STATUS "Compressing web/index.html -> index.html.gz")
    execute_process(
        COMMAND "${PYTHON_CMD}" "${GZIP_SCRIPT}" "${WEB_SRC}" "${WEB_GZ}"
        RESULT_VARIABLE GZIP_RESULT
        ERROR_VARIABLE GZIP_ERROR
    )
    if(NOT GZIP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to gzip web/index.html (exit code: ${GZIP_RESULT}, error: ${GZIP_ERROR})")
    endif()
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "startup_anim.cpp"
        "wifi_manager.cpp"
        "time_manager.cpp"
        "location_manager.cpp"
        "iana_tz.cpp"
        "geocoding.cpp"
        "weather.cpp"
        "status_bar.cpp"
        "api_server.cpp"
        "ota_manager.cpp"
        "auth_manager.cpp"
        "ha_integration.cpp"
        "setup_pairing.cpp"
        "ha_websocket.cpp"
        "app_preferences.cpp"
        "display_idle.cpp"
        "factory_reset.cpp"
        "advanced_params.cpp"
        "heatpump_errors.cpp"
        "event_log.cpp"
        "history_storage.cpp"
        "telemetry_history.cpp"
        "system_restart.cpp"
        "event_log_screen.cpp"
        "ui_dialog.cpp"
        "boot_stats.cpp"
        "wifi_supervisor.cpp"
        "log_buffer.cpp"
        "log_persist.cpp"
        "net_diag.cpp"
        "settings/settings_menu.cpp"
        "settings/settings_wifi_screen.cpp"
        "settings/keyboard_maps.c"
        "settings/settings_firmware_screen.cpp"
        "settings/settings_time_screen.cpp"
        "settings/settings_language_screen.cpp"
        "settings/settings_display_screen.cpp"
        "settings/settings_home_assistant_screen.cpp"
        "settings/settings_security_screen.cpp"
        "settings/settings_web_screen.cpp"
        "i18n/i18n.cpp"
        "fonts/montserrat_16_latin.c"
        "fonts/montserrat_24_latin.c"
        "fonts/montserrat_32_latin.c"
        "fonts/montserrat_40_latin.c"
        "fonts/montserrat_16_misc.c"
        "fonts/montserrat_24_misc.c"
        "fonts/montserrat_32_misc.c"
        "fonts/montserrat_40_misc.c"
        "fonts/weather_icons_32.c"
        "heatpump_controller.cpp"
        "tuya/tuya_listener.cpp"
        "tuya/macon_uart_transport.cpp"
        "tuya/macon_master.cpp"
        "heatpump_screen.cpp"
        "heatpump_temps_screen.cpp"
        "heatpump_history_screen.cpp"
        "heatpump_params_screen.cpp"
        "heatpump_errors_screen.cpp"
        "nav_bar.cpp"
        "tab_shell.cpp"
        "lv_mem_psram.c"
        "test_endpoints.cpp"
        "tls_manager.cpp"
        "png_encoder.c"
        "png_uncompressed.c"
    INCLUDE_DIRS
        "."
        "settings"
        "i18n"
        "fonts"
        "tuya"
    EMBED_FILES
        "web/index.html.gz"
    REQUIRES
        espressif__m5stack_tab5
        lvgl
        mooncake
        mooncake_log
        smooth_ui_toolkit
        esp_wifi
        esp_event
        esp_hosted
        nvs_flash
        esp_netif
        mdns
        esp_http_server
        esp_https_server
        cjson
        esp_https_ota
        app_update
        esp_http_client
        esp-tls
        mbedtls
        spi_flash
        arctic-macon
        lwip
        espcoredump
)

# Build-time: re-gzip whenever the source HTML or the gzip script changes, so
# `idf.py build` alone re-embeds the latest dashboard (no reconfigure needed).
add_custom_command(
    OUTPUT "${WEB_GZ}"
    COMMAND "${PYTHON_CMD}" "${GZIP_SCRIPT}" "${WEB_SRC}" "${WEB_GZ}"
    DEPENDS "${WEB_SRC}" "${GZIP_SCRIPT}"
    COMMENT "Gzipping web/index.html"
    VERBATIM
)
add_custom_target(web_assets DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} web_assets)

# Bake a per-build fingerprint into the firmware so CI can verify, after an OTA,
# that the device is actually running THE BUILD UNDER TEST and did not silently
# roll back to the previous good image. CI writes build_sha.txt (at the project
# root) before building; local builds default to "dev". A file is used rather
# than an env var because the ESP-IDF build runs in a container that does not
# forward arbitrary host environment variables. The define is attached directly
# to this component's target (not add_compile_definitions at the project root),
# because the root CMakeLists runs after project()/component registration, so a
# directory-scope define there never reaches these translation units.
if(EXISTS "${CMAKE_SOURCE_DIR}/build_sha.txt")
    file(READ "${CMAKE_SOURCE_DIR}/build_sha.txt" ARCTIC_BUILD_SHA)
    string(STRIP "${ARCTIC_BUILD_SHA}" ARCTIC_BUILD_SHA)
elseif(DEFINED ENV{ARCTIC_BUILD_SHA} AND NOT "$ENV{ARCTIC_BUILD_SHA}" STREQUAL "")
    set(ARCTIC_BUILD_SHA "$ENV{ARCTIC_BUILD_SHA}")
else()
    set(ARCTIC_BUILD_SHA "dev")
endif()
if("${ARCTIC_BUILD_SHA}" STREQUAL "")
    set(ARCTIC_BUILD_SHA "dev")
endif()
message(STATUS "ARCTIC_BUILD_SHA=${ARCTIC_BUILD_SHA}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE ARCTIC_BUILD_SHA="${ARCTIC_BUILD_SHA}")

# LVGL (a separate archive, liblvgl.a) references lv_malloc_core / lv_free_core /
# etc., which we provide from lv_mem_psram.c here in libmain.a. Because libmain
# is placed before liblvgl on the link line, the linker discards our object
# before it sees the references and fails with "undefined reference". Forcing
# the linker to keep lv_malloc_core pulls the whole lv_mem_psram.c object in
# early, so all of the lv_*_core symbols are defined for LVGL.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
