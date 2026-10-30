cmake_minimum_required(VERSION 3.16)
project(tschart_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(.. tschart)

add_executable(test_tschart_logic test_logic.cpp)
target_link_libraries(test_tschart_logic PRIVATE tschart_logic)
target_include_directories(test_tschart_logic PRIVATE ../src)
target_compile_options(test_tschart_logic PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME tschart_logic COMMAND test_tschart_logic)
