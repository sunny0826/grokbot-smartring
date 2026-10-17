idf_component_register(
    SRCS "main.cpp" "bot_motion.cpp" "bot_face.cpp" "bot_render.cpp" "bot_states.cpp" "bot_character.cpp" "bot_character_pika.cpp" "bot_character_rowlet.cpp" "character_presenter.cpp" "voice_character.cpp" "gadget.cpp" "gadget_wifi.cpp" "gadget_config.cpp" "gadget_clock.cpp" "gadget_ws.cpp" "status_bar.cpp"
         "voice.cpp" "voice_journal.cpp" "voice_mailbox.cpp" "voice_scope.cpp" "voice_transport.cpp" "voice_http.cpp" "voice_protocol.cpp" "voice_page_transport.cpp" "voice_ui.cpp" "voice_motion.cpp" "home_controls.cpp" "fonts/voice_font_22.c"
         "wifi_setup.cpp" "wifi_setup_driver.cpp" "wifi_setup_ui.cpp" "wifi_setup_keyboard.cpp"
         "wifi_setup_phone_ui.cpp" "wifi_setup_http.cpp" "wifi_setup_http_protocol.cpp"
    INCLUDE_DIRS "."
    REQUIRES smartring_plus esp_lvgl_adapter lvgl esp_timer cjson esp_driver_usb_serial_jtag esp_wifi esp-tls mdns nvs_flash esp_http_client esp_http_server tcp_transport
    EMBED_TXTFILES "wifi_setup_page.html" "wifi_setup_page.js" "wifi_setup_page.css"
)

if(MY_BOT_DISPLAY_BENCHMARK)
    target_sources(${COMPONENT_LIB} PRIVATE "../tools/render_smoke/device_benchmark.cpp")
    target_include_directories(${COMPONENT_LIB} PRIVATE "../tools/render_smoke")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MY_BOT_DISPLAY_BENCHMARK=1)
endif()
