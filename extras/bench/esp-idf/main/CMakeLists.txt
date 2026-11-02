# BAVA is optional and not part of this repository (GPLv3). To include it:
#   BAVA_DIR=/path/to/bavaprotocol idf.py build
# Only its src/*.c and include/bava.h are used, as a black box.
set(srcs "main.c" "../../bench_core.c")
set(incs "." "../..")
set(BAVA_DIR "$ENV{BAVA_DIR}")
if(BAVA_DIR)
    file(GLOB bava_srcs "${BAVA_DIR}/src/*.c")
    list(APPEND srcs ${bava_srcs} "../../bench_bava.c")
    list(APPEND incs "${BAVA_DIR}/include")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs}
                       PRIV_REQUIRES esp_driver_uart esp_timer freertos)

if(BAVA_DIR)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_WITH_BAVA=1)
    set(bava_opts "-w")
    if(CONFIG_BENCH_BAVA_BAREMETAL)
        list(APPEND bava_opts "-UESP_PLATFORM")
    endif()
    # bench_bava.c must see bava.h exactly as BAVA's own sources do
    set_source_files_properties(${bava_srcs} "../../bench_bava.c" PROPERTIES COMPILE_OPTIONS "${bava_opts}")
endif()
