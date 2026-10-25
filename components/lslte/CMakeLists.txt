# USB LTE stick driver (Huawei NCM). Built only with CONFIG_LS_LTE; the
# Kconfig value is unknown in the requirements pass, so the requirements
# are listed either way and only the sources depend on it.
if(CONFIG_LS_LTE)
    set(LS_LTE_SRCS ls_lte.c ls_ncm.c ls_lte_at.c ls_lte_console.c)
else()
    set(LS_LTE_SRCS "")
endif()
idf_component_register(
    SRCS ${LS_LTE_SRCS}
    INCLUDE_DIRS include
    REQUIRES esp_netif
    PRIV_REQUIRES usb esp_event esp_timer esp_hw_support console nvs_flash lsnet lakeshark)
if(CONFIG_LS_LTE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)
endif()
