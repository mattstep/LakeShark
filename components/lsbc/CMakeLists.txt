# The bc calculator engine: pure C, shared by the 'bc' console command
# (main/ls_bc_cmd.c) and the BC app (components/apps/tui/screens/ext/scr_bc.c).
# The bench compiles bc.c on its own with plain cc. With CONFIG_LS_BC off
# nothing is compiled; the include directory stays so a header reached by
# mistake still resolves.
set(LS_BC_SRCS "")
if(CONFIG_LS_BC)
    set(LS_BC_SRCS bc.c)
endif()

idf_component_register(
    SRCS ${LS_BC_SRCS}
    INCLUDE_DIRS .)

if(CONFIG_LS_BC)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wshadow)
endif()
