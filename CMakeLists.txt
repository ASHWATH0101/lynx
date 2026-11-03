# ESP-IDF component when built by idf.py, a plain static library otherwise.
# (Zephyr uses zephyr/CMakeLists.txt via zephyr/module.yml.)
# Keep the source list in sync with zephyr/CMakeLists.txt.
if(ESP_PLATFORM)
    idf_component_register(SRCS "src/lynx.c" "src/lynx_encode.c" "src/lynx_decode.c"
                                "src/lynx_dispatch.c" "src/lynx_txn.c"
                                "port/esp-idf/lynx_port.c"
                           INCLUDE_DIRS "src" "port/esp-idf"
                           REQUIRES freertos)
    # PUBLIC: application code must see the same lx_t layout as the library
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LX_PORT)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(lynx VERSION 0.1.0 LANGUAGES C)

add_library(lynx STATIC
    src/lynx.c
    src/lynx_encode.c
    src/lynx_decode.c
    src/lynx_dispatch.c
    src/lynx_txn.c)
add_library(lynx::lynx ALIAS lynx)
target_include_directories(lynx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(lynx PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_lynx test/test_lynx.c)
    target_link_libraries(test_lynx PRIVATE lynx)
    add_test(NAME test_lynx COMMAND test_lynx)
endif()
