# Multi-interface routing policy and the 'net' console command. Small and
# always built: with Wi-Fi alone it only pins what lwIP would pick anyway.
idf_component_register(
    SRCS ls_net.c ls_net_console.c
    INCLUDE_DIRS include
    REQUIRES esp_netif
    PRIV_REQUIRES console esp_event esp_wifi lwip esp_http_client nvs_flash lakeshark)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)
