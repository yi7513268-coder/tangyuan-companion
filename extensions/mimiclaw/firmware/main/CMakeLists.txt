set(MIMI "${CMAKE_CURRENT_LIST_DIR}/../upstream/mimiclaw-main")
set(PORT "${CMAKE_CURRENT_LIST_DIR}/../port")
idf_component_register(SRCS
    "${MIMI}/agent/agent_loop.c" "${MIMI}/bus/message_bus.c"
    "${PORT}/adapter.c" "${PORT}/task_tools.c"
    "board.c" "wire.c" "screen.cpp" "robot.c" "audio_io.c" "cached_audio.c" "computer_io.c" "interaction.c" "interaction_intent.c" "agent_extra.c" "relative_timer.c" "turn_trace.c" "local_dialogue.c" "voice_control.c"
    INCLUDE_DIRS "${PORT}" "${MIMI}" "."
    REQUIRES esp_wifi esp_netif esp_http_client esp_http_server esp-tls espressif__cjson nvs_flash
      esp_timer esp_event watche_hw_display watche_hw_body watche_hw_audio hal_camera)
