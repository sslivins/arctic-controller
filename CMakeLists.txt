# Arctic Controller
# ESP-IDF Project for M5Stack Tab5

cmake_minimum_required(VERSION 3.16)

# Project version - update this for releases
set(PROJECT_VER "2.11.6")

# Add dependencies to component search path
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/mooncake
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/mooncake_log
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/smooth_ui_toolkit
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(arctic_controller)
