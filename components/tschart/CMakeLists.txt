if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/tschart.cpp" "src/tschart_draw.cpp" "src/tschart_logic.cpp"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
        REQUIRES lvgl
        PRIV_REQUIRES esp_timer heap)
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(tschart_logic STATIC ${CMAKE_CURRENT_LIST_DIR}/src/tschart_logic.cpp)
    target_include_directories(tschart_logic PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_features(tschart_logic PUBLIC cxx_std_17)
    target_compile_options(tschart_logic PRIVATE -Wall -Wextra -Werror)
endif()
