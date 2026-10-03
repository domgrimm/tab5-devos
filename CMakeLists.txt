cmake_minimum_required(VERSION 3.16)

# Detect if building under ESP-IDF or Host Simulator
if(DEFINED ENV{IDF_PATH} AND NOT DEVOS_SIMULATOR)
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(tab5-devos)
else()
    # Host Simulator Build (Linux / macOS with SDL2 & LVGL v9)
    project(devos_sim C CXX)

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # Enable LV_CONF_INCLUDE_SIMPLE and include directory for lv_conf.h
    add_compile_definitions(LV_CONF_INCLUDE_SIMPLE=1)
    add_compile_definitions(DEVOS_SIMULATOR=1)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR})

    # Find SDL2
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})

    # Add LVGL v9 from components/lvgl
    add_subdirectory(components/lvgl)

    # Include directories for devOS components and apps
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/main/include
        ${CMAKE_CURRENT_SOURCE_DIR}/main
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_launcher
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_terminal
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_editor
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_tailscale
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_mqtt
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_wireguard
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_netdiag
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_rest
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_docker
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_adsb
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_cricket
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_totp
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_settings
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_template
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_coder
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_core
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_ui
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_storage
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_fileshare
        ${CMAKE_CURRENT_SOURCE_DIR}/components/bsp_tab5
        ${CMAKE_CURRENT_SOURCE_DIR}/components/tab5_keyboard
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_tailnet
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_net
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_sysmon
        ${CMAKE_CURRENT_SOURCE_DIR}/components/libssh2_port
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_vterm
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_json
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_mqtt
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_wireguard
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_qr
        ${CMAKE_CURRENT_SOURCE_DIR}/components/quirc
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_power
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_ota
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_mdview
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_http
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_netdiag
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_docker
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_adsb
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_maptiles
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_cricket
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_hashfile
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_actions
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_events
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_jobs
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_err
        ${CMAKE_CURRENT_SOURCE_DIR}/main/jobs_providers
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_totp
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_audio
    )

    # Sources
    set(DEVOS_SOURCES
        main/main.c
        main/apps/app_launcher/app_launcher.c
        main/apps/app_terminal/app_terminal.c
        main/apps/app_editor/app_editor.c
        main/apps/app_editor/ed_voice.c
        main/apps/app_tailscale/app_tailscale.c
        main/apps/app_mqtt/app_mqtt.c
        main/apps/app_wireguard/app_wireguard.c
        main/apps/app_netdiag/app_netdiag.c
        main/apps/app_netdiag/nd_ui_ping.c
        main/apps/app_netdiag/nd_ui_dns.c
        main/apps/app_netdiag/nd_ui_scan.c
        main/apps/app_netdiag/nd_ui_wifi.c
        main/apps/app_netdiag/nd_ui_mdns.c
        main/apps/app_netdiag/nd_ui_wol.c
        main/apps/app_rest/app_rest.c
        main/apps/app_docker/app_docker.c
        main/apps/app_adsb/app_adsb.c
        main/apps/app_adsb/adsb_map.c
        main/apps/app_cricket/app_cricket.c
        main/apps/app_totp/app_totp.c
        main/apps/app_settings/app_settings.c
        main/apps/app_template/app_template.c
        main/apps/app_coder/app_coder.c
        main/apps/app_coder/coder_core.c
        components/devos_core/devos_core.c
        components/devos_core/devos_apps.c
        components/devos_core/devos_clipboard.c
        components/devos_ui/devos_theme.c
        components/devos_ui/devos_top_bar.c
        components/devos_ui/devos_codeview.c
        components/devos_ui/devos_focus.c
        components/devos_ui/devos_widgets.c
        components/devos_ui/devos_qrscan.c
        components/devos_ui/devos_icons.c
        components/devos_ui/devos_match.c
        components/devos_ui/devos_cmdpal.c
        components/devos_ui/devos_hud.c
        components/devos_ui/devos_shortcuts.c
        components/devos_ui/devos_toast.c
        components/devos_ui/devos_powerdlg.c
        components/devos_ui/lv_font_nimbus_mono_14.c
        components/devos_storage/devos_storage.c
        components/devos_fileshare/devos_fileshare.c
        components/devos_fileshare/fileshare_page.c
        components/bsp_tab5/bsp_tab5.c
        components/tab5_keyboard/tab5_keyboard.c
        components/devos_tailnet/devos_tailnet.c
        components/devos_net/devos_net.c
        components/devos_sysmon/devos_sysmon.c
        components/libssh2_port/libssh2_port.c
        components/devos_vterm/devos_vterm.c
        components/devos_json/devos_json.c
        components/devos_mqtt/devos_mqtt.c
        components/devos_wireguard/devos_wireguard.c
        components/devos_qr/devos_qr.c
        components/bsp_tab5/bsp_tab5_camera.c
        components/quirc/quirc.c
        components/quirc/decode.c
        components/quirc/identify.c
        components/quirc/version_db.c
        components/devos_power/devos_power.c
        components/devos_ota/devos_ota.c
        components/devos_mdview/devos_mdview.c
        components/devos_http/devos_http.c
        components/devos_netdiag/nd_common.c
        components/devos_netdiag/nd_ping.c
        components/devos_netdiag/nd_dns.c
        components/devos_netdiag/nd_scan.c
        components/devos_netdiag/nd_wol.c
        components/devos_netdiag/nd_probe.c
        components/devos_docker/devos_docker.c
        components/devos_adsb/devos_adsb.c
        components/devos_maptiles/devos_maptiles.c
        components/devos_cricket/devos_cricket.c
        components/devos_crypto/devos_crypto.c
        components/devos_hashfile/devos_hashfile.c
        components/devos_actions/devos_actions.c
        components/devos_events/devos_events.c
        components/devos_jobs/jobs_model.c
        components/devos_jobs/jobs_parse.c
        components/devos_jobs/jobs_validate.c
        components/devos_jobs/jobs_serialize.c
        components/devos_jobs/jobs_platform.c
        components/devos_jobs/jobs_runtime.c
        components/devos_jobs/jobs_schedule.c
        components/devos_jobs/jobs_store.c
        main/jobs_providers/jobs_providers.c
        main/jobs_providers/jobs_system.c
        main/jobs_providers/jobs_http.c
        main/jobs_providers/jobs_network.c
        components/devos_totp/devos_totp.c
        components/devos_audio/devos_audio.c
    )

    add_executable(devos_sim ${DEVOS_SOURCES})
    target_link_libraries(devos_sim PRIVATE lvgl ${SDL2_LIBRARIES} m pthread)

    # HTTPS in the simulator needs the mbedTLS headers (mbedtls-devel, or
    # -DDEVOS_MBEDTLS_ROOT=<prefix with include/ and lib64/>). Without them
    # devos_http does plain http:// only.
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h HINTS ${DEVOS_MBEDTLS_ROOT}/include)
    find_library(MBEDTLS_LIB NAMES mbedtls libmbedtls.so.21 HINTS ${DEVOS_MBEDTLS_ROOT}/lib64 ${DEVOS_MBEDTLS_ROOT}/lib)
    find_library(MBEDX509_LIB NAMES mbedx509 libmbedx509.so.7 HINTS ${DEVOS_MBEDTLS_ROOT}/lib64 ${DEVOS_MBEDTLS_ROOT}/lib)
    find_library(MBEDCRYPTO_LIB NAMES mbedcrypto libmbedcrypto.so.16 HINTS ${DEVOS_MBEDTLS_ROOT}/lib64 ${DEVOS_MBEDTLS_ROOT}/lib)
    if(MBEDTLS_INCLUDE_DIR AND MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB)
        message(STATUS "devOS sim: HTTPS via mbedTLS (${MBEDTLS_INCLUDE_DIR})")
        set_source_files_properties(components/devos_http/devos_http.c PROPERTIES
            COMPILE_DEFINITIONS DEVOS_HTTP_TLS=1 INCLUDE_DIRECTORIES ${MBEDTLS_INCLUDE_DIR})
        target_link_libraries(devos_sim PRIVATE ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
    else()
        message(STATUS "devOS sim: no mbedTLS headers - http:// only")
    endif()

    # Output directory convenience
    set_target_properties(devos_sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
