# Zephyr module build (see zephyr/module.yml)
if(CONFIG_LYNX)
    set(LYNX_DIR ${ZEPHYR_CURRENT_MODULE_DIR})
    zephyr_library()
    # Keep the source list in sync with the top-level CMakeLists.txt.
    zephyr_library_sources(
        ${LYNX_DIR}/src/lynx.c
        ${LYNX_DIR}/src/lynx_encode.c
        ${LYNX_DIR}/src/lynx_decode.c
        ${LYNX_DIR}/src/lynx_dispatch.c
        ${LYNX_DIR}/src/lynx_txn.c
        ${LYNX_DIR}/port/zephyr/lynx_port.c)
    zephyr_include_directories(${LYNX_DIR}/src ${LYNX_DIR}/port/zephyr)
    # global: application code must see the same lx_t layout as the library
    zephyr_compile_definitions(LX_PORT)
endif()
