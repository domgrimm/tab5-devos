# THROWAWAY DIAGNOSTIC SWITCH: set to 1 to build the display-only firmware
# (main_display_test.c) which links ONLY the display stack so the 256 KB L2 cache
# fits. Set back to 0 for the normal firmware. Keep in sync with DEVOS_DISPLAY_TEST
# in the top-level CMakeLists.txt (which also applies sdkconfig.displaytest).
set(DEVOS_DISPLAY_TEST 0)

if(DEVOS_DISPLAY_TEST)
    idf_component_register(
        SRCS "main_display_test.c"
        INCLUDE_DIRS "." "include"
        REQUIRES
            bsp_tab5
            lvgl
    )
else()
set(APP_SRCS
    "main.c"
    "apps/app_launcher/app_launcher.c"
    "apps/app_opendev/app_opendev.c"
    "apps/app_terminal/app_terminal.c"
    "apps/app_editor/app_editor.c"
    "apps/app_tailscale/app_tailscale.c"
    "apps/app_antigravity/app_antigravity.c"
    "apps/app_settings/app_settings.c"
    "apps/app_template/app_template.c"
)

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS
        "."
        "include"
        "apps/app_launcher"
        "apps/app_opendev"
        "apps/app_terminal"
        "apps/app_editor"
        "apps/app_tailscale"
        "apps/app_antigravity"
        "apps/app_settings"
        "apps/app_template"
    REQUIRES
        devos_core
        devos_ui
        devos_config
        devos_storage
        devos_power
        devos_ota
        bsp_tab5
        tab5_keyboard
        devos_net
        microlink
        libssh2_port
        opendev_client
        agy_client
        devos_mdview
        lvgl
        nvs_flash
)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
