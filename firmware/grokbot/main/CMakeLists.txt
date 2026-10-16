idf_component_register(
    SRCS "main.cpp" "bot_face.cpp" "bot_render.cpp" "bot_states.cpp"
    INCLUDE_DIRS "."
    REQUIRES smartring_plus esp_lvgl_adapter lvgl esp_timer
)
