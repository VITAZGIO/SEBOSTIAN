#include "settings.h"
#include <string.h>
#include <math.h>
#include "nvs.h"
#include "esp_log.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef OTA_PASS
#define OTA_PASS ""
#endif
#ifndef SEB_HOST
#define SEB_HOST "192.168.1.201"
#endif
#ifndef SEB_PORT
#define SEB_PORT 9010
#endif

static const char *TAG = "set";
seb_settings_t g_set;
static seb_settings_t s_saved;

static void get_str(nvs_handle_t h, const char *k, char *dst, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, k, dst, &len) != ESP_OK) {
        strncpy(dst, def, cap - 1);
        dst[cap - 1] = 0;
    }
}

static int get_i32(nvs_handle_t h, const char *k, int def)
{
    int32_t v;
    return nvs_get_i32(h, k, &v) == ESP_OK ? (int)v : def;
}

void settings_load(void)
{
    nvs_handle_t h;
    bool ok = nvs_open("seb", NVS_READONLY, &h) == ESP_OK;
    memset(&g_set, 0, sizeof(g_set));
    if (ok) {
        get_str(h, "ssid", g_set.wifi_ssid, sizeof(g_set.wifi_ssid), WIFI_SSID);
        get_str(h, "wpass", g_set.wifi_pass, sizeof(g_set.wifi_pass), WIFI_PASS);
        get_str(h, "otapass", g_set.ota_pass, sizeof(g_set.ota_pass), OTA_PASS);
        get_str(h, "host", g_set.srv_host, sizeof(g_set.srv_host), SEB_HOST);
        g_set.srv_port = get_i32(h, "port", SEB_PORT);
        g_set.out_mode = get_i32(h, "out", OUT_BOTH);
        g_set.volume = get_i32(h, "vol", 30) / 100.0f;
        g_set.spk_ceil = get_i32(h, "ceil", 35) / 100.0f;
        g_set.wake_on = get_i32(h, "wake", 1) != 0;
        g_set.aec_on = get_i32(h, "aec", 1) != 0;
        g_set.ns_on = get_i32(h, "ns", 0) != 0;
        g_set.aec_lead_ms = get_i32(h, "aeclead", 10);
        nvs_close(h);
    } else {
        strncpy(g_set.wifi_ssid, WIFI_SSID, sizeof(g_set.wifi_ssid) - 1);
        strncpy(g_set.wifi_pass, WIFI_PASS, sizeof(g_set.wifi_pass) - 1);
        strncpy(g_set.ota_pass, OTA_PASS, sizeof(g_set.ota_pass) - 1);
        strncpy(g_set.srv_host, SEB_HOST, sizeof(g_set.srv_host) - 1);
        g_set.srv_port = SEB_PORT;
        g_set.out_mode = OUT_BOTH;
        g_set.volume = 0.30f;
        g_set.spk_ceil = 0.35f;
        g_set.wake_on = true;
        g_set.aec_on = true;
        g_set.ns_on = false;
        g_set.aec_lead_ms = 10;
    }
    if (g_set.out_mode < OUT_SPK || g_set.out_mode > OUT_BOTH) g_set.out_mode = OUT_BOTH;
    if (g_set.spk_ceil < 0.05f || g_set.spk_ceil > 1.0f) g_set.spk_ceil = 0.35f;
    if (g_set.volume < 0 || g_set.volume > g_set.spk_ceil + 0.001f) g_set.volume = fminf(0.30f, g_set.spk_ceil);
    s_saved = g_set;
    ESP_LOGI(TAG, "Wi-Fi \"%s\", сервер %s:%d, вывод %d, громкость %.2f (потолок %.2f), WAKE %d, AEC %d",
             g_set.wifi_ssid, g_set.srv_host, g_set.srv_port, g_set.out_mode, g_set.volume, g_set.spk_ceil,
             g_set.wake_on, g_set.aec_on);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("seb", NVS_READWRITE, &h) != ESP_OK) return;
#define SS(f, k) if (strcmp(g_set.f, s_saved.f)) nvs_set_str(h, k, g_set.f)
#define SI(f, k, x) if ((x)(g_set.f) != (x)(s_saved.f)) nvs_set_i32(h, k, (x)(g_set.f))
    SS(wifi_ssid, "ssid");
    SS(wifi_pass, "wpass");
    SS(ota_pass, "otapass");
    SS(srv_host, "host");
    SI(srv_port, "port", int32_t);
    SI(out_mode, "out", int32_t);
    if (lroundf(g_set.volume * 100) != lroundf(s_saved.volume * 100)) nvs_set_i32(h, "vol", lroundf(g_set.volume * 100));
    if (lroundf(g_set.spk_ceil * 100) != lroundf(s_saved.spk_ceil * 100)) nvs_set_i32(h, "ceil", lroundf(g_set.spk_ceil * 100));
    SI(wake_on, "wake", int32_t);
    SI(aec_on, "aec", int32_t);
    SI(ns_on, "ns", int32_t);
    SI(aec_lead_ms, "aeclead", int32_t);
#undef SS
#undef SI
    nvs_commit(h);
    nvs_close(h);
    s_saved = g_set;
}

bool settings_have_wifi(void) { return g_set.wifi_ssid[0] != 0; }
