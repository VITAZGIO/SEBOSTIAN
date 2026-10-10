// =====================================================================
//  СЕБАСТЬЯН — голова (ESP32-S3-N16R8), прошивка на ESP-IDF v5.5.5
//  Версия и история — firmware/common/include/seb_version.h
//
//  Порядок старта важен: сначала то, без чего не работает прошивка по Wi-Fi
//  (NVS, Wi-Fi, веб, /ota, сторож отката), и только потом всё остальное.
//  Если что-то из «остального» падает 3 раза подряд — безопасный режим (только Wi-Fi + OTA).
// =====================================================================
#include <stdio.h>
#include "board.h"
#include "state.h"
#include "settings.h"
#include "sysmon.h"
#include "net.h"
#include "web.h"
#include "power.h"
#include "display.h"
#include "ui.h"
#include "link.h"
#include "sdcard.h"
#include "audio.h"
#include "mp3.h"
#include "mic.h"
#include "voice.h"
#include "listener.h"
#include "app.h"
#include "seb_ota.h"
#include "seb_logbuf.h"
#include "seb_version.h"
#include "led_strip_encoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static const char *ota_pass(void) { return g_set.ota_pass; }
static bool ota_net_ok(void) { return net_connected() && web_running(); }
static void ota_progress(int pct, bool done)
{
    g_st.head_ota_pct = pct;          // экран показывает полноэкранный прогресс
    if (pct == 0) mp3_stop();
    if (pct < 0) {
        g_st.head_ota_pct = -1;
        st_flash(COL_RED, "OTA ОШИБКА");
    }
}

// бортовой WS2812 на GPIO48 — погасить (иначе может светить после сброса)
static void rgb_led_off(void)
{
    rmt_channel_handle_t ch = NULL;
    rmt_tx_channel_config_t cc = {
        .gpio_num = PIN_RGB_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    if (rmt_new_tx_channel(&cc, &ch) != ESP_OK) return;
    rmt_encoder_handle_t enc = NULL;
    led_strip_encoder_config_t ec = {.resolution = 10 * 1000 * 1000};
    rmt_new_led_strip_encoder(&ec, &enc);
    rmt_enable(ch);
    uint8_t px[3] = {0, 0, 0};
    rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(ch, enc, px, sizeof(px), &tc);
    rmt_tx_wait_all_done(ch, 100);
    rmt_disable(ch);
    rmt_del_channel(ch);
    rmt_del_encoder(enc);
}

static void early_gpio(void)
{
    // MAX98357A без валидного I2S шипят: мьют HIGH до запуска звука, AUX молчит
    gpio_config_t o = {.pin_bit_mask = (1ULL << PIN_SPK_MUTE) | (1ULL << PIN_XSMT), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&o);
    gpio_set_level(PIN_SPK_MUTE, 1);
    gpio_set_level(PIN_XSMT, 0);
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_AUX_DET, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
}

void app_main(void)
{
    seb_logbuf_init(32 * 1024, true);
    ESP_LOGW(TAG, "===== SEBASTIAN %s: HEAD (ESP32-S3) =====", SEB_HEAD_VERSION);
    sysmon_init();
    st_init();
    early_gpio();

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    power_boot_gate();                       // спали из-за аккума? может, спим дальше

    // ---- критично для прошивки по Wi-Fi ----
    seb_ota_cfg_t oc = {.get_password = ota_pass, .net_ok = ota_net_ok, .on_progress = ota_progress};
    seb_ota_guard_start(&oc);
#ifdef SEB_BROKEN_TEST
    // ТЕСТ ОТКАТА: эта сборка нарочно не включает Wi-Fi. Через 2 минуты должна вернуться прошлая прошивка.
    ESP_LOGE(TAG, "ТЕСТОВАЯ «БИТАЯ» ПРОШИВКА: Wi-Fi не запускаю — жди отката через %d с", SEB_OTA_DEADLINE_S);
    st_flash(COL_RED, "ТЕСТ ОТКАТА: жди 2 мин");
#else
    net_start();
    web_start();
#endif

    if (sysmon_safe_mode()) {
        ESP_LOGE(TAG, "БЕЗОПАСНЫЙ РЕЖИМ: работают только Wi-Fi, веб и OTA. Залей исправленную прошивку.");
        return;
    }

    // ---- всё остальное ----
    rgb_led_off();
    if (display_init() == ESP_OK) {
        ui_init();
        display_backlight(true);
    }
    link_start();
    g_st.aux_plug = gpio_get_level(PIN_AUX_DET) == 1;
    g_st.sd_ok = sdcard_init();
    audio_init();
    mp3_init();
    g_st.mic_ok = mic_init();
    ESP_LOGI(TAG, "микрофон I2S0: %s", g_st.mic_ok ? "OK" : "ОШИБКА");
    voice_init();
    listener_init();
    app_start();
#ifdef SEB_BROKEN_TEST
    for (;;) { st_flash(COL_RED, "ТЕСТ ОТКАТА: жди 2 мин"); vTaskDelay(pdMS_TO_TICKS(1500)); }
#endif
    ESP_LOGW(TAG, "готово. Сенсоры: 1 = тише, 2 = play/stop, 3 = громче. RAM %u, PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
