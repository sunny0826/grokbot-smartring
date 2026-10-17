target_sources(smoke PRIVATE
    "${MAIN}/character_presenter.cpp" "${MAIN}/voice_character.cpp"
    "${MAIN}/bot_render.cpp" "${MAIN}/bot_face.cpp" "${MAIN}/bot_states.cpp"
    "${MAIN}/bot_character.cpp" "${MAIN}/bot_character_pika.cpp" "${MAIN}/bot_character_rowlet.cpp")
target_include_directories(smoke PRIVATE "${FIRMWARE}/sim/shim" "${FIRMWARE}/tools/voice_ui_smoke")
