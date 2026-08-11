idf_component_register(
    SRCS
        "main.c"
        "../src/loopback.c"
        "../src/wifi_loopback.c"
    INCLUDE_DIRS
        "."
        "../inc"
    REQUIRES
        wsm_driver
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        lwip
)

# NOTE: do NOT define WSM_DRIVER_SOCKET_WRAP here. wifi_loopback.c keys off it to
# bind __real_lwip_* (bypassing the W5500 --wrap), and the wsm_driver component
# already defines it PUBLIC from CONFIG_WSM_DRIVER_SOCKET_WRAP, so it arrives via
# REQUIRES. Hardcoding it breaks the ETH backend, where the wrap is off and
# __real_lwip_* do not exist.

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
    -Wno-unused-function
    )
