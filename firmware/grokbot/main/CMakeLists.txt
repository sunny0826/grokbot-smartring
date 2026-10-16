idf_component_register(
    SRCS "main.cpp" "bot_face.cpp" "bot_render.cpp" "bot_states.cpp" "bot_character.cpp" "bot_character_pika.cpp" "bot_character_rowlet.cpp" "gadget.cpp" "gadget_wifi.cpp" "status_bar.cpp"
    INCLUDE_DIRS "."
    REQUIRES smartring_plus esp_lvgl_adapter lvgl esp_timer cjson esp_driver_usb_serial_jtag esp_wifi esp-tls mdns nvs_flash
)
