# Native (host) unit tests for host-testable sources under main/.
#
# Build & run:
#   cmake -S main/tests -B build-main-tests
#   cmake --build build-main-tests
#   ctest --test-dir build-main-tests --output-on-failure
#
# Why this is self-contained rather than a native branch in main/CMakeLists.txt:
# main/ is the firmware component and its CMakeLists is consumed by the IDF
# build on every device build. Keeping the host build here means a mistake in
# the test harness can never break the firmware build, and the set of sources
# considered host-testable is stated in one obvious place (MAIN_HOST_SRCS
# below) instead of behind an if(NOT ESP_PLATFORM) that nobody reads.
#
# Adding a source: put it in MAIN_HOST_SRCS, add whatever IDF headers it needs
# to host_stubs/, and add an add_executable/add_test pair. Prefer extending an
# existing stub over weakening a source's dependencies to suit the host build.

cmake_minimum_required(VERSION 3.16)
project(arctic_main_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Sources from main/ compiled for the host. Keep this list small and honest:
# a source belongs here only if the host build exercises its real logic, not a
# hollowed-out version of it.
set(MAIN_HOST_SRCS
  ${MAIN_DIR}/i18n/i18n.cpp
  ${MAIN_DIR}/history_storage.cpp
)

add_library(arctic_main_host STATIC
  ${MAIN_HOST_SRCS}
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/nvs_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_partition_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_crc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/heap_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_err.cpp
)

# host_stubs must come first so its esp_log.h/nvs.h shadow nothing real; there
# is no IDF on the host, so ordering is about intent rather than necessity.
target_include_directories(arctic_main_host PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${MAIN_DIR}
  ${MAIN_DIR}/i18n
)

target_compile_options(arctic_main_host PRIVATE -Wall -Wextra -Wno-unused-parameter)

enable_testing()

add_executable(test_i18n test_i18n.cpp)
target_link_libraries(test_i18n PRIVATE arctic_main_host)
add_test(NAME i18n COMMAND test_i18n)

add_executable(test_nvs_fake test_nvs_fake.cpp)
target_link_libraries(test_nvs_fake PRIVATE arctic_main_host)
add_test(NAME nvs_fake COMMAND test_nvs_fake)

add_executable(test_history_storage test_history_storage.cpp)
target_link_libraries(test_history_storage PRIVATE arctic_main_host)
add_test(NAME history_storage COMMAND test_history_storage)
