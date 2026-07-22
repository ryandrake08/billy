# The HAL is still stubs (common components only). The transport layer now brings up WiFi and
# talks HTTP, so it needs the WiFi/event/netif/nvs stack plus esp_http_client. Add more REQUIRES
# (esp_driver_i2s, esp_driver_gpio, esp_driver_ledc, ...) as the HAL gains real drivers in Stage 3.
idf_component_register(
    SRCS "main.c" "runloop.c" "hal.c" "net.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client
)

# WiFi credentials and the backend host come from the build environment, never source control:
# export WIFI_SSID, WIFI_PASSWORD, and BACKEND_HOST before building and they're injected as
# compile definitions. BACKEND_HOST is required (no default — the topology stays out of the repo).
# Note: CMake reads these at configure time, so after changing an exported value run
# `idf.py reconfigure` (or `set-target`) to pick it up.
set(WIFI_SSID "$ENV{WIFI_SSID}")
set(WIFI_PASSWORD "$ENV{WIFI_PASSWORD}")
set(BACKEND_HOST "$ENV{BACKEND_HOST}")
if(BACKEND_HOST STREQUAL "")
    message(FATAL_ERROR
        " BACKEND_HOST is not set. Point the firmware at your backend host, then reconfigure:\n"
        "     export BACKEND_HOST=\"your-host-or-ip\"\n"
        "     idf.py reconfigure build")
endif()
if(WIFI_SSID STREQUAL "")
    message(WARNING
        " WIFI_SSID is not set — the firmware will build but cannot join WiFi at runtime.\n"
        " Set the credentials in your build environment, then reconfigure:\n"
        "     export WIFI_SSID=\"your-ssid\"\n"
        "     export WIFI_PASSWORD=\"your-password\"\n"
        "     idf.py reconfigure build")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
    BACKEND_HOST="${BACKEND_HOST}")
