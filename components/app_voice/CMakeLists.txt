idf_component_register(SRCS "app_voice.c" "voice_audio.c" "voice_bot.c" "voice_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES app_buddy esp_common
    PRIV_REQUIRES app_alarm app_config app_wake esp-mosaico-bsp esp_wifi esp_netif esp_event esp_timer
        esp_http_client mbedtls log espressif__esp_websocket_client espressif__cjson)

# Optional build-time secrets in projects/grok_desk_buddy/secrets/ (git-ignored):
#   xai_api_key  - fallback when Vibe Mode has no xai_api_key shared setting
#   relay_url    - grok-buddy-relay base URL (enables the Grok Bot hand-off)
#   relay_token  - relay device token
#   wifi2_ssid / wifi2_password - backup network (e.g. phone hotspot)
# A missing file embeds an empty placeholder so its symbols always exist.
set(SECRETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../secrets")
file(MAKE_DIRECTORY "${SECRETS_DIR}")
# configure_file tracks existing files; watching the folder (its mtime changes
# when a file is added or removed) makes a new secret trigger reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SECRETS_DIR}")
foreach(secret xai_api_key relay_url relay_token wifi2_ssid wifi2_password)
    set(src "${SECRETS_DIR}/${secret}")
    set(dst "${CMAKE_CURRENT_BINARY_DIR}/${secret}")
    if(EXISTS "${src}")
        configure_file("${src}" "${dst}" COPYONLY)
    else()
        file(WRITE "${dst}" "")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${dst}" TEXT)
endforeach()
