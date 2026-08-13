# Arctic Controller
# ESP-IDF Project for M5Stack Tab5

cmake_minimum_required(VERSION 3.16)

# Project version - update this for releases
set(PROJECT_VER "2.10.3")

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

# Bake a per-build fingerprint into the firmware so CI can verify, after an OTA,
# that the device is actually running THE BUILD UNDER TEST and did not silently
# roll back to the previous good image. CI writes build_sha.txt (e.g. the commit
# SHA) before building; local builds default to "dev". A file is used rather than
# an env var because the ESP-IDF build runs in a container that does not forward
# arbitrary host environment variables.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build_sha.txt")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/build_sha.txt" ARCTIC_BUILD_SHA)
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
add_compile_definitions(ARCTIC_BUILD_SHA="${ARCTIC_BUILD_SHA}")
