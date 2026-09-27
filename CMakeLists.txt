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
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_settings
        ${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_template
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_core
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_ui
        ${CMAKE_CURRENT_SOURCE_DIR}/components/devos_storage
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
    )

    # Sources
    set(DEVOS_SOURCES
        main/main.c
        main/apps/app_launcher/app_launcher.c
        main/apps/app_terminal/app_terminal.c
        main/apps/app_editor/app_editor.c
        main/apps/app_tailscale/app_tailscale.c
        main/apps/app_mqtt/app_mqtt.c
        main/apps/app_wireguard/app_wireguard.c
        main/apps/app_settings/app_settings.c
        main/apps/app_template/app_template.c
        components/devos_core/devos_core.c
        components/devos_ui/devos_theme.c
        components/devos_ui/devos_top_bar.c
        components/devos_ui/devos_codeview.c
        components/devos_ui/devos_focus.c
        components/devos_ui/lv_font_nimbus_mono_14.c
        components/devos_storage/devos_storage.c
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
    )

    add_executable(devos_sim ${DEVOS_SOURCES})
    target_link_libraries(devos_sim PRIVATE lvgl ${SDL2_LIBRARIES} m pthread)

    # Output directory convenience
    set_target_properties(devos_sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
