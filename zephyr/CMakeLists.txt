# Zephyr module build (see zephyr/module.yml)
if(CONFIG_LYNX)
    set(LYNX_DIR ${ZEPHYR_CURRENT_MODULE_DIR})
    zephyr_library()
    zephyr_library_sources(${LYNX_DIR}/lynx.c ${LYNX_DIR}/port/zephyr/lynx_port.c)
    zephyr_include_directories(${LYNX_DIR} ${LYNX_DIR}/port/zephyr)
    # global: application code must see the same lx_t layout as lynx.c
    zephyr_compile_definitions(LX_PORT)
endif()
