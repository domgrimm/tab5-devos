set(APP_SRCS
    "main.c"
    "apps/app_launcher/app_launcher.c"
    "apps/app_terminal/app_terminal.c"
    "apps/app_editor/app_editor.c"
    "apps/app_tailscale/app_tailscale.c"
    "apps/app_settings/app_settings.c"
    "apps/app_template/app_template.c"
)

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS
        "."
        "include"
        "apps/app_launcher"
        "apps/app_terminal"
        "apps/app_editor"
        "apps/app_tailscale"
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
        devos_sysmon
        devos_tailnet
        libssh2_port
        devos_vterm
        devos_mdview
        lvgl
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
