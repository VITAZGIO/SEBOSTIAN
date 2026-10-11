file(GLOB FONTS "fonts/*.c")
idf_component_register(
    SRCS "main.c" "state.c" "settings.c" "sysmon.c" "net.c" "web.c" "display.c" "ui.c" "app.c" "link.c"
         "audio.c" "alink.c" "mp3.c" "fx.c" "mic.c" "voice.c" "listener.c" "train.c" "batlog.c" "power.c" "sdcard.c"
         ${FONTS}
    INCLUDE_DIRS "."
    REQUIRES common lvgl libhelix-mp3 mdns sr_lite esp_lcd esp_driver_spi esp_driver_i2s esp_driver_gpio
             esp_driver_uart esp_driver_rmt esp_driver_sdspi fatfs sdmmc esp_wifi esp_netif esp_http_server
             nvs_flash app_update espcoredump json esp_psram esp_timer lwip)

# Тестовая «битая» прошивка для проверки автоотката: idf.py -B build_broken -DSEB_BROKEN_TEST=1 build
if(SEB_BROKEN_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEB_BROKEN_TEST=1)
endif()
