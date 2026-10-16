idf_component_register(
    SRCS "main.cpp" "bot_face.cpp" "bot_render.cpp" "bot_states.cpp" "bot_character.cpp" "bot_character_pika.cpp" "bot_character_rowlet.cpp"
    INCLUDE_DIRS "."
    REQUIRES smartring_plus esp_lvgl_adapter lvgl esp_timer
)
