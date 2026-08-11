idf_component_register(
    SRCS
        "main.c"
        "../src/ssl_echo.c"
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
        mbedtls
        esp_hw_support
)

# NOTE: do NOT define WSM_DRIVER_SOCKET_WRAP here. The wsm_driver component
# already defines it PUBLIC from CONFIG_WSM_DRIVER_SOCKET_WRAP, so it arrives
# via REQUIRES. Hardcoding it breaks the ETH backend, where the wrap is off.

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
    -Wno-unused-function
    )
