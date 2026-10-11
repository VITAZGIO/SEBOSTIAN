idf_component_register(
    SRCS "main.c" "led.c" "rf433.c" "btaudio.c" "alink.c" "battery.c" "link.c" "otamode.c"
    INCLUDE_DIRS "."
    REQUIRES common bt mdns esp_driver_i2s esp_driver_gpio esp_driver_uart esp_driver_rmt esp_adc
             esp_wifi esp_netif esp_http_server nvs_flash app_update esp_timer esp_ringbuf)
