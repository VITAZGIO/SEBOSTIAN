#include "otamode.h"
#include <string.h>
#include "cp.h"
#include "led.h"
#include "rf433.h"
#include "btaudio.h"
#include "seb_ota.h"
#include "seb_logbuf.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
#define OTA_TIMEOUT_MS (5 * 60 * 1000)

static volatile bool s_req;
static volatile int s_state;          // 1 подключаюсь, 2 жду, 3 прошиваюсь, 4 готово, 9 ошибка, 0 выход
static volatile int s_pct;
static char s_ip[20] = "-";
static volatile bool s_conn;
static httpd_handle_t s_srv;

void ota_mode_request(void) { s_req = true; }
bool ota_mode_requested(void) { return s_req; }

static void report(void) { link_send("W,%d,%d,%s", s_state, s_pct, s_ip); }

static const char *get_pw(void) { return g_cfg.ota; }
static bool net_ok(void) { return s_conn && s_srv; }
static void on_progress(int pct, bool done)
{
    if (pct < 0) { s_state = 9; report(); return; }
    s_state = done ? 4 : 3;
    int old = s_pct;
    s_pct = pct;
    if (done || pct / 5 != old / 5) report();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { s_conn = false; esp_wifi_connect(); }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_conn = true;
    }
}

void ota_mode_run(bool verify)
{
    g_ota_mode = true;
    s_state = 1;
    s_pct = 0;
    ESP_LOGW(TAG, "%s: выключаю Bluetooth, подключаюсь к Wi-Fi...", verify ? "проверка новой прошивки" : "режим прошивки");
    report();
    rf433_rx_enable(false);
    led_fill(120, 0, 160);                     // фиолетовая = режим прошивки
    bta_bt_stop();
    seb_ota_cfg_t oc = {.get_password = get_pw, .net_ok = net_ok, .on_progress = on_progress};
    seb_ota_guard_start(&oc);
    if (!g_cfg.ssid[0]) {
        ESP_LOGE(TAG, "нет настроек Wi-Fi: голова (IDF) присылает их строкой CFG после старта");
        s_state = 9;
    } else {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, COPROC_HOSTNAME);
        wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&wc);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        wifi_config_t c = {0};
        memcpy(c.sta.ssid, g_cfg.ssid, strnlen(g_cfg.ssid, sizeof(c.sta.ssid)));
        memcpy(c.sta.password, g_cfg.pass, strnlen(g_cfg.pass, sizeof(c.sta.password) - 1));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &c);
        esp_wifi_start();
    }
    int64_t t0 = now_ms(), t_rep = 0, t_err = 0;
    for (;;) {
        if (s_state == 1 && s_conn) {
            httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
            hc.stack_size = 6144;
            if (httpd_start(&s_srv, &hc) == ESP_OK) {
                seb_ota_register(s_srv);
                seb_logbuf_register(s_srv);
            }
            if (mdns_init() == ESP_OK) mdns_hostname_set(COPROC_HOSTNAME);
            s_state = 2;
            ESP_LOGW(TAG, "жду прошивку: http://%s/ota (%s.local), 5 минут", s_ip, COPROC_HOSTNAME);
        } else if (s_state == 1 && now_ms() - t0 > 20000) {
            s_state = 9;
            ESP_LOGE(TAG, "Wi-Fi не подключился (SSID/пароль?)");
        }
        if (now_ms() - t_rep > 500) { t_rep = now_ms(); report(); }
        if (s_state == 9 && !t_err) t_err = now_ms();
        // проверка новой прошивки: как только сторож пометил её рабочей — в обычный режим
        if (verify && s_state == 2 && !seb_ota_pending() && now_ms() - t0 > 5000) {
            ESP_LOGW(TAG, "новая прошивка проверена — перезагрузка в обычный режим");
            break;
        }
        // выход: ошибка -> через 3 с, никто не прошил за 5 мин -> перезагрузка в обычный режим с BT
        if ((s_state == 9 && t_err && now_ms() - t_err > 3000 && !seb_ota_pending()) ||
            (s_state == 2 && now_ms() - t0 > OTA_TIMEOUT_MS && !seb_ota_active())) {
            ESP_LOGW(TAG, "выхожу из режима прошивки — перезагрузка");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_state = 0;
    report();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
