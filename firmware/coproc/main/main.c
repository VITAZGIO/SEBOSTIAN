// =====================================================================
//  СЕБАСТЬЯН — со-процессор (ESP32 D0WD-V3), прошивка на ESP-IDF v5.5.5
//  Версия и история — firmware/common/include/seb_version.h
//  Bluetooth-колонка «Sebastian» (A2DP) -> PCM5102A (AUX), мелодии, ARGB, 3 сенсора TTP223,
//  кнопка мика, батарея + защита, радио 433, UART к голове, OTA отдельным режимом.
// =====================================================================
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "cp.h"
#include "led.h"
#include "rf433.h"
#include "btaudio.h"
#include "battery.h"
#include "otamode.h"
#include "seb_ota.h"
#include "seb_logbuf.h"
#include "seb_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

cp_cfg_t g_cfg;
volatile int g_t[3];
volatile int g_mic_btn;
volatile int g_bat_mv;
volatile bool g_ota_mode;

static RTC_NOINIT_ATTR uint32_t s_crash, s_magic;

// ---------------- NVS ----------------
void cfg_load(void)
{
    nvs_handle_t h;
    memset(&g_cfg, 0, sizeof(g_cfg));
    if (nvs_open("seb", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(g_cfg.ssid);
    nvs_get_str(h, "ssid", g_cfg.ssid, &l);
    l = sizeof(g_cfg.pass);
    nvs_get_str(h, "wpass", g_cfg.pass, &l);
    l = sizeof(g_cfg.ota);
    nvs_get_str(h, "otapass", g_cfg.ota, &l);
    nvs_close(h);
}

void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("seb", NVS_READWRITE, &h) != ESP_OK) return;
    char old[65];
    size_t l = sizeof(old);       // пишем только если изменилось (флеш не любит лишних записей)
    if (nvs_get_str(h, "ssid", old, &l) != ESP_OK || strcmp(old, g_cfg.ssid)) nvs_set_str(h, "ssid", g_cfg.ssid);
    l = sizeof(old);
    if (nvs_get_str(h, "wpass", old, &l) != ESP_OK || strcmp(old, g_cfg.pass)) nvs_set_str(h, "wpass", g_cfg.pass);
    l = sizeof(old);
    if (nvs_get_str(h, "otapass", old, &l) != ESP_OK || strcmp(old, g_cfg.ota)) nvs_set_str(h, "otapass", g_cfg.ota);
    nvs_commit(h);
    nvs_close(h);
}

bool lowflag_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("seb", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "lowbat", &v); nvs_close(h); }
    return v != 0;
}

void lowflag_set(bool v)
{
    if (lowflag_get() == v) return;
    nvs_handle_t h;
    if (nvs_open("seb", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "lowbat", v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------- сенсоры и кнопка (антидребезг: 2 одинаковых чтения по 20 мс) ----------------
static void poll_inputs(void)
{
    static const int PINS[3] = {PIN_T1, PIN_T2, PIN_T3};
    static int cnt[3];
    for (int i = 0; i < 3; i++) {
        int r = gpio_get_level(PINS[i]);              // TTP223: HIGH = касание
        if (r != g_t[i]) {
            if (++cnt[i] >= 2) {
                cnt[i] = 0;
                g_t[i] = r;
                ESP_LOGI(TAG, "сенсор %d: %s", i + 1, r ? "НАЖАТ" : "отпущен");
                if (r) {
                    link_send("T,%d", i + 1);         // событие для головы
                    static const uint8_t C[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
                    led_fill(C[i][0], C[i][1], C[i][2]);    // сразу видно на ленте
                }
            }
        } else cnt[i] = 0;
    }
    int mb = gpio_get_level(PIN_MIC_BTN) == 0;
    if (mb != g_mic_btn) { g_mic_btn = mb; ESP_LOGI(TAG, "кнопка мика: %s", mb ? "НАЖАТА" : "отпущена"); }
}

static void inputs_init(void)
{
    gpio_config_t t = {.pin_bit_mask = (1ULL << PIN_T1) | (1ULL << PIN_T2) | (1ULL << PIN_T3), .mode = GPIO_MODE_INPUT};
    gpio_config(&t);
    gpio_config_t b = {.pin_bit_mask = 1ULL << PIN_MIC_BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&b);
}

void app_main(void)
{
    seb_logbuf_init(8 * 1024, false);
    ESP_LOGW(TAG, "===== SEBASTIAN %s: CO-PROCESSOR (ESP32) =====", SEB_COPROC_VERSION);
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || s_magic != 0x5EB0C0) { s_crash = 0; s_magic = 0x5EB0C0; }
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) s_crash++;

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    cfg_load();
    link_init();
    bat_boot_gate();                                  // спали из-за аккума? может, спим дальше

    inputs_init();
    led_init();
    rf433_init();
    bta_init();

    // новая прошивка после OTA или 3 падения подряд — сразу режим прошивки (Wi-Fi + /ota)
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending || s_crash >= 3) {
        if (s_crash >= 3) ESP_LOGE(TAG, "3 падения подряд — БЕЗОПАСНЫЙ РЕЖИМ: только Wi-Fi + OTA");
        s_crash = 0;
        link_send("HELLO coproc");
        link_send("V,%s", SEB_COPROC_VERSION);
        ota_mode_run(pending);
    }

    bta_bt_start();
    bta_beep();                                       // при старте AUX должен пискнуть
    g_bat_mv = bat_read_now();
    ESP_LOGI(TAG, "аккум %.2f В", g_bat_mv / 1000.0f);
    link_send("HELLO coproc");
    link_send("V,%s", SEB_COPROC_VERSION);
    if (!g_cfg.ssid[0]) link_send("CFG?");            // попросить у головы Wi-Fi для будущей OTA
    ESP_LOGW(TAG, "готово. Трогай сенсоры, жми кнопки на экране головы.");

    int64_t t_in = 0, t_bat = 0, t_stat = 0, t_log = 0;
    int last_bt = -1;
    for (;;) {
        int64_t t = now_ms();
        if (ota_mode_requested()) ota_mode_run(false);    // не возвращается
        if (t - t_in >= 20) { t_in = t; poll_inputs(); }
        led_tick();
        int bt = bta_bt_state();
        if (bt != last_bt) {
            if (bt == 1 && last_bt == 0) led_fill(0, 0, 255);      // подключился — лента синяя
            last_bt = bt;
        }
        if (t - t_bat >= 1000) { t_bat = t; bat_tick(); }
        if (t - t_stat >= 200) {
            t_stat = t;
            link_send("S,%d,%d,%d,%d,%d,%lu,%d,%d", g_t[0], g_t[1], g_t[2], g_mic_btn, g_bat_mv,
                      (unsigned long)(t / 1000), bt, bta_music_on() ? 1 : 0);
        }
        if (t - t_log >= 2000) {
            t_log = t;
            if (t > 60000) s_crash = 0;
            ESP_LOGI(TAG, "T:%d%d%d мик:%d акк:%.2fВ BT:%s музыка:%d | от головы: %s (PING %lu)", g_t[0], g_t[1], g_t[2],
                     g_mic_btn, g_bat_mv / 1000.0f, bt == 2 ? "play" : bt == 1 ? "conn" : "wait", bta_music_on(),
                     (g_last_ping_ms && t - g_last_ping_ms < 2500) ? "OK" : "НЕТ (провод S3 8 -> ESP 16?)",
                     (unsigned long)g_pings);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
