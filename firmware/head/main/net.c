#include "net.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "state.h"

static const char *TAG = "net";
static esp_netif_t *s_sta, *s_ap;
static volatile bool s_conn, s_ap_on, s_started_services;
static volatile int64_t s_disc_since;
static volatile bool s_stopping, s_need_retry;

static void start_services(void)
{
    if (s_started_services) return;
    s_started_services = true;
    setenv("TZ", "MSK-3", 1);
    tzset();
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "ntp1.stratum2.ru"));
    c.start = true;
    esp_netif_sntp_init(&c);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(NET_HOSTNAME);
        mdns_instance_name_set("Sebastian head");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    ESP_LOGI(TAG, "часы (NTP, MSK-3) и mDNS %s.local запущены", NET_HOSTNAME);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (settings_have_wifi()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_conn) ESP_LOGW(TAG, "связь потеряна (причина %d), переподключаюсь...", d->reason);
        s_conn = false;
        s_need_retry = true;
        if (!s_disc_since) s_disc_since = now_ms();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "подключён: IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_conn = true;
        s_disc_since = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "к точке доступа подключились — настройка: http://192.168.4.1/setup");
    }
}

static void start_ap(void)
{
    if (s_ap_on) return;
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, NET_AP_SSID);
    ap.ap.ssid_len = strlen(NET_AP_SSID);
    strcpy((char *)ap.ap.password, NET_AP_PASS);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    esp_wifi_set_mode(settings_have_wifi() ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_on = true;
    ESP_LOGW(TAG, "точка доступа \"%s\" (пароль %s): http://192.168.4.1/setup", NET_AP_SSID, NET_AP_PASS);
}

static void net_task(void *arg)
{
    int64_t t_retry = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_stopping) continue;
        if (!settings_have_wifi()) { start_ap(); continue; }
        if (s_conn && !s_started_services) start_services();
        if (!s_conn) {
            int64_t t = now_ms();
            if (s_need_retry && t - t_retry > 5000) {     // переподключение раз в 5 с после обрыва
                t_retry = t;
                s_need_retry = false;
                esp_wifi_connect();
            }
            if (s_disc_since && t - s_disc_since > 180000) start_ap();
        } else if (s_ap_on) {                     // связь есть — точка доступа больше не нужна
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_on = false;
            ESP_LOGI(TAG, "Wi-Fi есть — точка доступа выключена");
        }
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, NET_HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, g_set.wifi_ssid, strnlen(g_set.wifi_ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, g_set.wifi_pass, strnlen(g_set.wifi_pass, sizeof(sta.sta.password) - 1));
    sta.sta.threshold.authmode = g_set.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);              // без энергосбережения: меньше задержки для голоса
    s_disc_since = now_ms();
    if (settings_have_wifi()) ESP_LOGI(TAG, "подключаюсь к \"%s\"...", g_set.wifi_ssid);
    else ESP_LOGW(TAG, "Wi-Fi не настроен");
    xTaskCreatePinnedToCore(net_task, "net", 3072, NULL, 4, NULL, 0);
}

bool net_connected(void) { return s_conn; }
bool net_ap_active(void) { return s_ap_on; }

void net_ip_str(char *out, size_t cap)
{
    esp_netif_ip_info_t ip;
    if (s_conn && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    else if (s_ap_on) snprintf(out, cap, "192.168.4.1 (AP)");
    else snprintf(out, cap, "---");
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_conn && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool net_time_valid(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year > 120;
}

void net_stop(void)
{
    s_stopping = true;
    esp_wifi_disconnect();
    esp_wifi_stop();
}
