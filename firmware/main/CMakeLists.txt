# Stubs only depend on common components (log, freertos, esp_common), which the main
# component gets implicitly. Add REQUIRES (esp_wifi, esp_http_client, nvs_flash, esp_driver_i2s,
# esp_driver_gpio, esp_driver_ledc, ...) here as the HAL and transport layers gain real drivers.
idf_component_register(
    SRCS "main.c" "runloop.c" "hal.c" "net.c"
    INCLUDE_DIRS "."
)
