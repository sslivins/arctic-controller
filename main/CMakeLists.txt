# Web UI gzip compression
# Generate gzipped web file at configure time if source is newer
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz")
set(GZIP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py")

# Check if we need to regenerate the gzip file
if(EXISTS "${WEB_SRC}")
    if(NOT EXISTS "${WEB_GZ}" OR "${WEB_SRC}" IS_NEWER_THAN "${WEB_GZ}")
        message(STATUS "Compressing web/index.html -> index.html.gz")
        # Find Python - prefer IDF's Python, then system Python
        # Use Python3::Interpreter if available (set by ESP-IDF), otherwise find it
        if(Python3_EXECUTABLE)
            set(PYTHON_CMD "${Python3_EXECUTABLE}")
        else()
            find_program(PYTHON_CMD NAMES python3 python)
        endif()
        if(NOT PYTHON_CMD)
            message(FATAL_ERROR "Python not found - required for gzip compression")
        endif()
        execute_process(
            COMMAND "${PYTHON_CMD}" "${GZIP_SCRIPT}" "${WEB_SRC}" "${WEB_GZ}"
            RESULT_VARIABLE GZIP_RESULT
            ERROR_VARIABLE GZIP_ERROR
        )
        if(NOT GZIP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to gzip web/index.html (exit code: ${GZIP_RESULT}, error: ${GZIP_ERROR})")
        endif()
    endif()
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "startup_anim.cpp"
        "wifi_manager.cpp"
        "time_manager.cpp"
        "status_bar.cpp"
        "time_screen.cpp"
        "api_server.cpp"
        "ota_manager.cpp"
        "auth_manager.cpp"
        "settings/settings_screen.cpp"
        "settings/settings_wifi_panel.cpp"
        "settings/settings_firmware_panel.cpp"
        "settings/settings_time_panel.cpp"
        "settings/settings_language_panel.cpp"
        "settings/settings_display_panel.cpp"
        "i18n/i18n.cpp"
        "fonts/montserrat_16_latin.c"
        "fonts/montserrat_24_latin.c"
        "modbus/modbus_manager.cpp"
        "modbus/arctic_heatpump.cpp"
    INCLUDE_DIRS
        "."
        "settings"
        "i18n"
        "fonts"
        "modbus"
    EMBED_FILES
        "web/index.html.gz"
    REQUIRES
        m5stack_tab5
        lvgl
        mooncake
        mooncake_log
        smooth_ui_toolkit
        esp_wifi
        esp_event
        nvs_flash
        esp_netif
        mdns
        esp_http_server
        json
        esp_https_ota
        app_update
        esp_http_client
        esp-tls
        mbedtls
)
