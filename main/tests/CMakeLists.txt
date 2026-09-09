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
  ${MAIN_DIR}/auth_manager.cpp
  ${MAIN_DIR}/log_buffer.cpp
  ${MAIN_DIR}/event_log.cpp
  ${MAIN_DIR}/log_persist.cpp
  ${MAIN_DIR}/boot_stats.cpp
  ${MAIN_DIR}/time_manager.cpp
  ${MAIN_DIR}/iana_tz.cpp
  ${MAIN_DIR}/location_manager.cpp
)

add_library(arctic_main_host STATIC
  ${MAIN_HOST_SRCS}
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/nvs_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_partition_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_crc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/heap_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_err.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/psa_crypto.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_random.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_log_hook.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/esp_rom_crc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/task_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/event_group_fake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/sntp_fake.cpp
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

# auth_manager spawns threads: its whole point is contending the integration-
# token mutex, which a single-threaded test cannot do.
find_package(Threads REQUIRED)
add_executable(test_auth_manager test_auth_manager.cpp)
target_link_libraries(test_auth_manager PRIVATE arctic_main_host Threads::Threads)
add_test(NAME auth_manager COMMAND test_auth_manager)

add_executable(test_log_buffer test_log_buffer.cpp)
target_link_libraries(test_log_buffer PRIVATE arctic_main_host)
add_test(NAME log_buffer COMMAND test_log_buffer)

# time_fake overrides libc's time(), so it is linked ONLY here rather than into
# arctic_main_host: no other suite should get a doctored wall clock. event_log
# treats a pre-2024 clock as "not synced", and that branch (plus the backfill
# that repairs it) is unreachable with the host's real clock.
add_executable(test_event_log test_event_log.cpp host_stubs/time_fake.cpp)
target_link_libraries(test_event_log PRIVATE arctic_main_host)
add_test(NAME event_log COMMAND test_event_log)

# log_persist forks a child per phase to model a reboot, and the flash fake's
# arena is a MAP_SHARED mmap so the "flash" survives the fork. The power-loss
# sweep runs one phase per cut point.
add_executable(test_log_persist test_log_persist.cpp)
target_link_libraries(test_log_persist PRIVATE arctic_main_host)
add_test(NAME log_persist COMMAND test_log_persist)

# boot_stats decides whether a crashing device keeps crashing. Each scenario
# forks because boot_stats_init() is single-shot per process -- one child is
# one boot -- and a "previous boot" is modelled by seeding the two NVS
# counters, which are the only state that crosses a reboot.
add_executable(test_boot_stats test_boot_stats.cpp)
target_link_libraries(test_boot_stats PRIVATE arctic_main_host)
add_test(NAME boot_stats COMMAND test_boot_stats)

# time_manager needs the doctored clock for the same reason event_log does, but
# for the opposite branch: is_synced() must be able to read a pre-2024 clock so
# that "the RTC was reset after the sync bit was latched" is reachable. Each
# scenario forks because time_mgr_init() is single-shot and TZ is process-wide.
add_executable(test_time_manager test_time_manager.cpp host_stubs/time_fake.cpp)
target_link_libraries(test_time_manager PRIVATE arctic_main_host)
add_test(NAME time_manager COMMAND test_time_manager)

# The auto-timezone path: a curated 66-entry IANA -> POSIX table, plus the
# policy deciding when to apply it. No time_fake here on purpose -- the table
# is checked by resolving each POSIX rule at fixed instants through the C
# library's own parser, so a doctored clock would only obscure things.
add_executable(test_location test_location.cpp)
target_link_libraries(test_location PRIVATE arctic_main_host)
add_test(NAME location COMMAND test_location)
