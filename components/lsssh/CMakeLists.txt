# SSH session engine, NVS key store and the vendored terminal emulator.
# Shared by main/ls_ssh.c (the 'ssh' console command) and the SSH app in
# components/apps. With CONFIG_LS_SSH off nothing is compiled; the include
# directories stay so a header reached by mistake still resolves.
set(LS_SSH_SRCS "")
if(CONFIG_LS_SSH)
    set(LS_SSH_SRCS ls_ssh_session.c ls_ssh_store.c third_party/libtmt/tmt.c)
endif()

idf_component_register(
    SRCS ${LS_SSH_SRCS}
    INCLUDE_DIRS include third_party/libtmt
    REQUIRES nvs_flash
    PRIV_REQUIRES esp_hw_support)

# libssh is not a plain requirement: requirements are resolved before Kconfig
# values exist, and a stock build leaves libssh out altogether (see the
# top-level CMakeLists.txt). Linked here, at registration, when it is there.
if(CONFIG_LS_SSH)
    idf_component_optional_requires(PRIVATE david-cermak__libssh)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)
endif()
