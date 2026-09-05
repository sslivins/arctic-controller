# Arctic Controller
# ESP-IDF Project for M5Stack Tab5

cmake_minimum_required(VERSION 3.16)

# Project version - update this for releases
set(PROJECT_VER "2.11.21")

# Add dependencies to component search path
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/mooncake
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/mooncake_log
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/smooth_ui_toolkit
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL 9.5 trips -Werror=attributes on GCC 15.2 (shipped with ESP-IDF 6.1).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=attributes" APPEND)
# ESP-IDF 6.1 turns on -Wextra by default. httpd_uri_t gained four new members
# (is_websocket, handle_ws_control_frames, supported_subprotocol,
# ws_pre_handshake_cb); omitted designated-initializer members are
# value-initialized, which is well defined. Keep the warning visible but
# non-fatal rather than churning ~280 initializers.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

project(arctic_controller)
