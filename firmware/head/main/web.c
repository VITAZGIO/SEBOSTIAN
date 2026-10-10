#include "web.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "state.h"
#include "settings.h"
#include "net.h"
#include "audio.h"
#include "mp3.h"
#include "mic.h"
#include "sdcard.h"
#include "sysmon.h"
#include "batlog.h"
#include "display.h"
#include "voice.h"
#include "train.h"
#include "seb_ota.h"
#include "seb_logbuf.h"
#include "seb_util.h"
#include "seb_version.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;

bool web_running(void) { return s_srv != NULL; }

// ---------------- / ----------------
static esp_err_t root_get(httpd_req_t *req)
{
    char ip[32], slot[48];
    net_ip_str(ip, sizeof(ip));
    seb_ota_describe(slot, sizeof(slot));
    char *h = malloc(3072);
    if (!h) return ESP_FAIL;
    snprintf(h, 3072,
             "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
             "<title>Sebastian head</title></head><body style='font-family:sans-serif;max-width:640px'>"
             "<h2>Себастьян — голова</h2>"
             "<p>Прошивка <b>%s</b>, слот %s%s. IP %s. Аптайм %lu с. Старт: %s.</p>"
             "<p>Сэмплы слова: POS %d, NEG %d, ФОН %d — <a href='/kws.tar'>скачать kws.tar</a></p>"
             "<p>Лог аккумулятора: %lu строк с запуска — <a href='/bat.tar'>скачать bat.tar</a> · "
             "<a href='/bat_clear?yes=1'>стереть</a></p>"
             "<p><a href='/setup'>Настройки</a> · <a href='/status'>status (JSON)</a> · <a href='/log'>лог</a> · "
             "<a href='/screenshot'>снимок экрана</a> · <a href='/coredump'>coredump</a></p>"
             "<p>Прошивка по Wi-Fi: <code>curl -u ota:ПАРОЛЬ --data-binary @sebastian_head.bin http://%s/ota</code></p>"
             "</body></html>",
             SEB_HEAD_VERSION, slot, seb_ota_pending() ? " (на проверке)" : "", ip, (unsigned long)(now_ms() / 1000),
             sysmon_reason_name(), g_st.kws_pos, g_st.kws_neg, g_st.kws_bg, (unsigned long)g_st.batlog_lines, ip);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, h);
    free(h);
    return ESP_OK;
}

// ---------------- /status ----------------
static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    char b[96];
    cJSON_AddStringToObject(j, "version", SEB_HEAD_VERSION);
    cJSON_AddStringToObject(j, "idf", esp_app_get_description()->idf_ver);
    seb_ota_describe(b, sizeof(b));
    cJSON_AddStringToObject(j, "slot", b);
    cJSON_AddBoolToObject(j, "safe_mode", sysmon_safe_mode());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(now_ms() / 1000));
    cJSON_AddStringToObject(j, "reset", sysmon_reason_name());
    cJSON_AddNumberToObject(j, "boot", sysmon_boot_count());
    cJSON *m = cJSON_AddObjectToObject(j, "mem");
    cJSON_AddNumberToObject(m, "int_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "int_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "int_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(m, "psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(m, "dma_free", heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    net_ip_str(b, sizeof(b));
    cJSON_AddStringToObject(w, "ip", b);
    cJSON_AddBoolToObject(w, "connected", net_connected());
    cJSON_AddNumberToObject(w, "rssi", net_rssi());
    cJSON_AddBoolToObject(w, "ap", net_ap_active());
    cJSON *a = cJSON_AddObjectToObject(j, "audio");
    cJSON_AddBoolToObject(a, "mp3", mp3_playing());
    cJSON_AddNumberToObject(a, "volume", g_set.volume);
    cJSON_AddNumberToObject(a, "spk_ceil", g_set.spk_ceil);
    cJSON_AddNumberToObject(a, "out_mode", g_set.out_mode);
    cJSON_AddBoolToObject(a, "spk_on", audio_spk_on());
    cJSON_AddBoolToObject(a, "aux_on", audio_aux_on());
    cJSON_AddBoolToObject(a, "aux_plug", g_st.aux_plug);
    cJSON_AddBoolToObject(a, "sd", sdcard_ok());
    cJSON_AddNumberToObject(a, "mic_level", g_st.mic_level);
    cJSON *e = cJSON_AddObjectToObject(j, "aec");
    cJSON_AddBoolToObject(e, "ok", g_st.aec_ok);
    cJSON_AddBoolToObject(e, "on", mic_aec_active());
    cJSON_AddNumberToObject(e, "erle_db", g_st.aec_erle_db);
    cJSON_AddNumberToObject(e, "echo_delay_ms", g_st.aec_delay_ms);
    cJSON_AddNumberToObject(e, "ref_level", g_st.aec_ref_lvl);
    cJSON_AddNumberToObject(e, "lead_ms", g_set.aec_lead_ms);
    cJSON_AddBoolToObject(e, "ns", g_set.ns_on);
    cJSON *v = cJSON_AddObjectToObject(j, "voice");
    cJSON_AddBoolToObject(v, "busy", voice_busy());
    cJSON_AddBoolToObject(v, "wake_on", g_set.wake_on);
    cJSON_AddBoolToObject(v, "listening", g_st.ls_running && !g_st.ls_paused);
    cJSON_AddNumberToObject(v, "noise", g_st.ls_nf);
    cJSON_AddNumberToObject(v, "level", g_st.ls_lvl);
    st_lock();
    cJSON_AddStringToObject(v, "state", g_st.v_state);
    cJSON_AddStringToObject(v, "you", g_st.v_you);
    cJSON_AddStringToObject(v, "seb", g_st.v_seb);
    cJSON_AddStringToObject(v, "heard", g_st.v_heard);
    cJSON_AddStringToObject(v, "t1", g_st.v_t1);
    cJSON_AddStringToObject(v, "t2", g_st.v_t2);
    st_unlock();
    cJSON *c = cJSON_AddObjectToObject(j, "coproc");
    cJSON_AddBoolToObject(c, "rx_ok", link_rx_ok());
    cJSON_AddBoolToObject(c, "tx_ok", link_tx_ok());
    cJSON_AddStringToObject(c, "version", g_st.cp_ver);
    cJSON_AddNumberToObject(c, "bat_mv", g_st.cp_bat);
    cJSON_AddNumberToObject(c, "bt", g_st.cp_bt);
    cJSON_AddNumberToObject(c, "ota", g_st.cp_ota);
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t nt = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = malloc(sizeof(TaskStatus_t) * (nt + 4));
    if (ts) {
        uint32_t total = 0;
        nt = uxTaskGetSystemState(ts, nt + 4, &total);
        cJSON *tk = cJSON_AddArrayToObject(j, "tasks");
        for (UBaseType_t i = 0; i < nt; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", ts[i].pcTaskName);
            cJSON_AddNumberToObject(o, "prio", ts[i].uxCurrentPriority);
            cJSON_AddNumberToObject(o, "stack_free", ts[i].usStackHighWaterMark);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            cJSON_AddNumberToObject(o, "cpu_pct", total ? (double)ts[i].ulRunTimeCounter * 100.0 / total : 0);
#endif
            cJSON_AddItemToArray(tk, o);
        }
        free(ts);
    }
#endif
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return ESP_OK;
}

// ---------------- /setup ----------------
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    char *tmp = malloc(cap * 3 + 4);
    if (!tmp) return false;
    bool ok = httpd_query_key_value(body, key, tmp, cap * 3 + 4) == ESP_OK;
    if (ok) {
        url_decode(tmp);
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
    }
    free(tmp);
    return ok;
}

static esp_err_t setup_get(httpd_req_t *req)
{
    char *h = malloc(4096);
    if (!h) return ESP_FAIL;
    bool need_pw = g_set.ota_pass[0] != 0;
    snprintf(h, 4096,
             "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>Setup</title></head>"
             "<body style='font-family:sans-serif;max-width:480px'><h2>Себастьян — настройки</h2>"
             "<form method='POST' action='/setup'>"
             "<p>Wi-Fi (SSID)<br><input name='ssid' value='%s' style='width:100%%'></p>"
             "<p>Пароль Wi-Fi (пусто = не менять)<br><input name='pass' type='password' style='width:100%%'></p>"
             "<p>Новый пароль OTA (пусто = не менять)<br><input name='ota' type='password' style='width:100%%'></p>"
             "<p>Сервер Себастьяна: адрес и порт<br><input name='host' value='%s'> <input name='port' value='%d' size='6'></p>"
             "<p>Потолок громкости динамиков (0.05–1.0, хрипят ≥ 0.5)<br><input name='ceil' value='%.2f' size='6'></p>"
             "<p>AEC: опережение reference, мс (подстройка эхоподавления)<br><input name='lead' value='%d' size='6'></p>"
             "%s"
             "<p><button>Сохранить</button> (при смене Wi-Fi — перезагрузка)</p></form>"
             "<p><a href='/'>назад</a></p></body></html>",
             g_set.wifi_ssid, g_set.srv_host, g_set.srv_port, g_set.spk_ceil, g_set.aec_lead_ms,
             need_pw ? "<p><b>Текущий пароль OTA</b> (обязательно)<br><input name='cur' type='password' style='width:100%'></p>" : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, h);
    free(h);
    return ESP_OK;
}

static esp_err_t setup_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 2048) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    char *body = malloc(len + 1);
    if (!body) return ESP_FAIL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[len] = 0;
    char cur[65] = "";
    form_get(body, "cur", cur, sizeof(cur));
    if (g_set.ota_pass[0] && strcmp(cur, g_set.ota_pass)) {
        free(body);
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "неверный текущий пароль OTA");
    }
    char ssid[33] = "", pass[65] = "", ota[65] = "", host[64] = "", tmp[16] = "";
    bool wifi_changed = false;
    if (form_get(body, "ssid", ssid, sizeof(ssid)) && strcmp(ssid, g_set.wifi_ssid)) {
        strcpy(g_set.wifi_ssid, ssid);
        wifi_changed = true;
    }
    if (form_get(body, "pass", pass, sizeof(pass)) && pass[0]) { strcpy(g_set.wifi_pass, pass); wifi_changed = true; }
    if (form_get(body, "ota", ota, sizeof(ota)) && ota[0]) strcpy(g_set.ota_pass, ota);
    if (form_get(body, "host", host, sizeof(host)) && host[0]) strcpy(g_set.srv_host, host);
    if (form_get(body, "port", tmp, sizeof(tmp)) && atoi(tmp) > 0) g_set.srv_port = atoi(tmp);
    if (form_get(body, "ceil", tmp, sizeof(tmp))) {
        float c = strtof(tmp, NULL);
        if (c >= 0.05f && c <= 1.0f) g_set.spk_ceil = c;
        if (g_set.volume > g_set.spk_ceil) g_set.volume = g_set.spk_ceil;
    }
    if (form_get(body, "lead", tmp, sizeof(tmp))) {
        int l = atoi(tmp);
        if (l >= -100 && l <= 100) g_set.aec_lead_ms = l;
    }
    free(body);
    settings_save();
    ESP_LOGW(TAG, "настройки сохранены%s", wifi_changed ? " (Wi-Fi изменён — перезагрузка)" : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, wifi_changed ? "<meta charset='utf-8'>Сохранено. Перезагружаюсь и подключаюсь к новому Wi-Fi..."
                                         : "<meta charset='utf-8'>Сохранено. <a href='/'>назад</a>");
    if (wifi_changed) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

// ---------------- tar с SD ----------------
static esp_err_t send_tar(httpd_req_t *req, const char *const *dirs, int nd, const char *fname)
{
    if (!sdcard_ok()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD");
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=%s", fname);
    httpd_resp_set_type(req, "application/x-tar");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    static uint8_t hdr[512];
    static const uint8_t zeros[512];
    if (!buf) return ESP_FAIL;
    uint32_t total = 0;
    for (int di = 0; di < nd; di++) {
        char dpath[48];
        snprintf(dpath, sizeof(dpath), SD_MOUNT "/%s", dirs[di]);
        DIR *d = opendir(dpath);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_type == DT_DIR) continue;
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", dpath, e->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            struct stat st;
            stat(path, &st);
            char name[300];
            snprintf(name, sizeof(name), "%s/%s", dirs[di], e->d_name);
            seb_tar_header(hdr, name, (uint32_t)st.st_size, 1790000000u);
            httpd_resp_send_chunk(req, (const char *)hdr, 512);
            size_t n;
            while ((n = fread(buf, 1, 8192, f)) > 0) httpd_resp_send_chunk(req, (const char *)buf, n);
            fclose(f);
            if (st.st_size % 512) httpd_resp_send_chunk(req, (const char *)zeros, 512 - st.st_size % 512);
            total += 512 + ((st.st_size + 511) / 512) * 512;
        }
        closedir(d);
    }
    httpd_resp_send_chunk(req, (const char *)zeros, 512);
    httpd_resp_send_chunk(req, (const char *)zeros, 512);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    ESP_LOGI(TAG, "%s отдан, %lu байт", fname, (unsigned long)total + 1024);
    return ESP_OK;
}

static esp_err_t kws_get(httpd_req_t *req)
{
    static const char *D[3] = {"kws/pos", "kws/neg", "kws/bg"};
    return send_tar(req, D, 3, "kws.tar");
}

static esp_err_t bat_get(httpd_req_t *req)
{
    static const char *D[1] = {"batlog"};
    batlog_flush();
    return send_tar(req, D, 1, "bat.tar");
}

static esp_err_t bat_clear_get(httpd_req_t *req)
{
    char q[32], v[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "yes", v, sizeof(v));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (strcmp(v, "1")) return httpd_resp_sendstr(req, "добавь ?yes=1, чтобы стереть /batlog");
    batlog_flush();
    int n = 0;
    DIR *d = opendir(SD_MOUNT "/batlog");
    if (d) {
        struct dirent *e;
        char path[320];
        while ((e = readdir(d))) {
            if (e->d_type == DT_DIR) continue;
            snprintf(path, sizeof(path), SD_MOUNT "/batlog/%s", e->d_name);
            if (remove(path) == 0) n++;
        }
        closedir(d);
    }
    g_st.batlog_lines = 0;
    char b[48];
    snprintf(b, sizeof(b), "удалено файлов: %d", n);
    return httpd_resp_sendstr(req, b);
}

// ---------------- снимок экрана / coredump ----------------
static esp_err_t shot_get(httpd_req_t *req)
{
    size_t len = 0;
    uint8_t *b = display_snapshot_bmp(&len);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no screen");
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send(req, (const char *)b, len);
    free(b);
    return ESP_OK;
}

static esp_err_t coredump_get(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "дампа нет (падений не было)");
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p) return ESP_FAIL;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=coredump.bin");
    uint8_t *buf = malloc(4096);
    if (!buf) return ESP_FAIL;
    size_t off = addr - p->address;
    for (size_t done = 0; done < size;) {
        size_t k = size - done > 4096 ? 4096 : size - done;
        esp_partition_read(p, off + done, buf, k);
        httpd_resp_send_chunk(req, (const char *)buf, k);
        done += k;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "reboot\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.max_uri_handlers = 16;
    c.lru_purge_enable = true;
    c.core_id = 0;
    c.recv_wait_timeout = 15;
    c.send_wait_timeout = 15;
    if (httpd_start(&s_srv, &c) != ESP_OK) { ESP_LOGE(TAG, "HTTP-сервер не стартовал"); s_srv = NULL; return; }
    httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/setup", .method = HTTP_GET, .handler = setup_get},
        {.uri = "/setup", .method = HTTP_POST, .handler = setup_post},
        {.uri = "/kws.tar", .method = HTTP_GET, .handler = kws_get},
        {.uri = "/bat.tar", .method = HTTP_GET, .handler = bat_get},
        {.uri = "/bat_clear", .method = HTTP_GET, .handler = bat_clear_get},
        {.uri = "/screenshot", .method = HTTP_GET, .handler = shot_get},
        {.uri = "/coredump", .method = HTTP_GET, .handler = coredump_get},
        {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s_srv, &u[i]);
    seb_ota_register(s_srv);
    seb_logbuf_register(s_srv);
    ESP_LOGI(TAG, "веб-сервер на порту 80 (/, /status, /setup, /ota, /log)");
}
