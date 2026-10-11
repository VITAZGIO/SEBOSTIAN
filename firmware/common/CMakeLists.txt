# Общий компонент обеих прошивок: протокол UART, «чистые» утилиты (тесты на ПК: test/),
# OTA с автооткатом, лог в памяти, кодировщик ленты WS2812.
idf_component_register(
    SRCS "src/seb_proto.c" "src/seb_text.c" "src/seb_media.c" "src/seb_dsp.c" "src/seb_rc.c" "src/seb_alink.c"
         "src/seb_hc.c" "src/seb_ota.c" "src/seb_logbuf.c" "src/led_strip_encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt esp_http_server app_update mbedtls esp_timer log lwip)
