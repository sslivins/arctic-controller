# Web UI gzip compression
# Generate gzipped web file at configure time if source is newer
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz")

# Convert to native paths for Python
file(TO_NATIVE_PATH "${WEB_SRC}" WEB_SRC_NATIVE)
file(TO_NATIVE_PATH "${WEB_GZ}" WEB_GZ_NATIVE)

# Check if we need to regenerate the gzip file
if(EXISTS "${WEB_SRC}")
    if(NOT EXISTS "${WEB_GZ}" OR "${WEB_SRC}" IS_NEWER_THAN "${WEB_GZ}")
        message(STATUS "Compressing web/index.html -> index.html.gz")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py" "${WEB_SRC}" "${WEB_GZ}"
            RESULT_VARIABLE GZIP_RESULT
        )
        if(NOT GZIP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to gzip web/index.html (exit code: ${GZIP_RESULT})")
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
        "i18n/i18n.cpp"
        "fonts/montserrat_16_latin.c"
        "fonts/montserrat_24_latin.c"
    INCLUDE_DIRS
        "."
        "settings"
        "i18n"
        "fonts"
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
