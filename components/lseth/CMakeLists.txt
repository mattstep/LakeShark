# Wired Ethernet (IP101 on the P4 EMAC). Sources only with CONFIG_LS_ETH;
# the requirements are listed either way because IDF resolves them before
# Kconfig values exist (see main/CMakeLists.txt).
set(LS_ETH_SRCS "")
if(CONFIG_LS_ETH)
    set(LS_ETH_SRCS ls_eth.c ls_eth_console.c)
endif()

idf_component_register(
    SRCS ${LS_ETH_SRCS}
    INCLUDE_DIRS .
    REQUIRES esp_netif esp_eth
    PRIV_REQUIRES lakeshark console esp_event esp_timer esp_hw_support nvs_flash esp_driver_gpio)

if(CONFIG_LS_ETH)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)
endif()
