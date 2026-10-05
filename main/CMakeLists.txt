idf_component_register(
    SRCS "app_main.c" "wifi.c" "timesync.c" "ecowitt.c" "poller.c" "display.c" "ui.c" "ui_assets.c"
    INCLUDE_DIRS "."
    REQUIRES crowpanel_epd driver esp_wifi esp_netif esp_event esp_timer
             esp_http_client mbedtls json nvs_flash lwip
)

# Secrets: read KEY=value lines from the gitignored creds files in the project
# root and generate secrets.h into the build directory (never into the source tree).
function(read_cred file key out_var)
    set(path "${CMAKE_SOURCE_DIR}/${file}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing ${file} in the project root (KEY=value lines)")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(STRINGS "${path}" lines REGEX "^${key}=")
    if(NOT lines)
        message(FATAL_ERROR "${file}: no '${key}=' line")
    endif()
    list(GET lines 0 line)
    string(REGEX REPLACE "^${key}=" "" value "${line}")
    string(STRIP "${value}" value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

read_cred(wifi_creds SSID WIFI_SSID)
read_cred(wifi_creds PASSWORD WIFI_PASSWORD)
read_cred(ecowitt_creds api_key ECOWITT_API_KEY)
read_cred(ecowitt_creds app_key ECOWITT_APP_KEY)
configure_file(secrets.h.in "${CMAKE_CURRENT_BINARY_DIR}/secrets.h" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
