idf_component_register(
    SRCS
        "main.c"
        "../src/pppoe_session.c"
        "PPPoE.c"
        "md5.c"
    INCLUDE_DIRS
        "."
        "../inc"
    REQUIRES
        wsm_driver
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
    -Wno-unused-function
    )

# PPPoE.h does `#include "socket.h"`, meaning ioLibrary's Ethernet/socket.h and
# its 4-argument socket(sn, protocol, port, flag). But ESP-IDF's lwIP port puts
# .../lwip/port/esp32xx/include/sys directly on the -I path, and that directory
# holds socket.h too, so the quote-form include resolves to lwIP's instead --
# leaving PPPoE.c looking at the 3-argument POSIX socket() ("too many arguments
# to function 'socket'"). -iquote is searched before every -I dir for
# quote-form includes, which fixes the resolution; the wsm_driver component
# applies the same trick to wiznet_toe.c for exactly this reason.
set(WIZNET_IOLIB_ETHERNET
    "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/ioLibrary_Driver/Ethernet")

# Vendored example sources from WIZnet-PICO-C; keep warnings non-fatal.
set_source_files_properties("PPPoE.c" PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-w;-iquote;${WIZNET_IOLIB_ETHERNET}")
set_source_files_properties("md5.c" PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-w")

# pppoe_session.c includes PPPoE.h as well, so it needs the same resolution.
# close=wiz_close matches what the component does for its own ioLibrary TU:
# ioLibrary's socket.h declares close(uint8_t), which would clash with newlib's
# close(int). This file never calls close(), so the rename is purely defensive.
set_source_files_properties("../src/pppoe_session.c" PROPERTIES
    COMPILE_DEFINITIONS "close=wiz_close"
    COMPILE_OPTIONS "-iquote;${WIZNET_IOLIB_ETHERNET}")
