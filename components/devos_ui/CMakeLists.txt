idf_component_register(
    SRCS "devos_theme.c" "devos_top_bar.c" "devos_codeview.c" "devos_focus.c" "devos_widgets.c" "devos_qrscan.c" "devos_icons.c" "devos_match.c" "devos_cmdpal.c" "devos_hud.c" "devos_shortcuts.c" "devos_toast.c" "devos_powerdlg.c" "lv_font_nimbus_mono_14.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_config lvgl
    PRIV_REQUIRES nvs_flash devos_qr devos_fileshare devos_power
)
