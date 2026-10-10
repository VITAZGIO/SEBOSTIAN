#include "seb_ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static seb_ota_cfg_t s_cfg;
static volatile bool s_pending = false;
static volatile bool s_active = false;

static void guard_task(void *arg)
{
    int ok_since = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_pending) break;
        int up = (int)(esp_timer_get_time() / 1000000);
        bool ok = s_cfg.net_ok && s_cfg.net_ok();
        if (ok && ok_since < 0) ok_since = up;
        if (!ok) ok_since = -1;
        if (ok_since >= 0 && up - ok_since >= 5 && up >= SEB_OTA_STABLE_S) {
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "новая прошивка помечена РАБОЧЕЙ (%s)", esp_err_to_name(e));
            s_pending = false;
            break;
        }
        if (up >= SEB_OTA_DEADLINE_S && !s_active) {
            ESP_LOGE(TAG, "за %d с не поднялись Wi-Fi/веб — ОТКАТ на прошлую прошивку", SEB_OTA_DEADLINE_S);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
    vTaskDelete(NULL);
}

void seb_ota_guard_start(const seb_ota_cfg_t *cfg)
{
    s_cfg = *cfg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        ESP_LOGW(TAG, "прошивка НА ПРОВЕРКЕ: жду Wi-Fi + веб %d с, иначе откат", SEB_OTA_DEADLINE_S);
        xTaskCreate(guard_task, "ota_guard", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGI(TAG, "прошивка рабочая (слот %s)", run ? run->label : "?");
    }
}

bool seb_ota_pending(void) { return s_pending; }
bool seb_ota_active(void) { return s_active; }

void seb_ota_describe(char *out, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    const char *sn = st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" :
                     st == ESP_OTA_IMG_NEW ? "new" : st == ESP_OTA_IMG_UNDEFINED ? "factory/cable" : "?";
    snprintf(out, cap, "%s (%s)", run ? run->label : "?", sn);
}

static bool check_auth(httpd_req_t *req)
{
    const char *pw = s_cfg.get_password ? s_cfg.get_password() : NULL;
    if (!pw || !*pw) return false;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", hdr, sizeof(hdr)) == ESP_OK)
        return strcmp(hdr, pw) == 0;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && !strncmp(hdr, "Basic ", 6)) {
        unsigned char dec[128];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            const char *c = strchr((char *)dec, ':');     // логин:пароль, логин любой
            return c && strcmp(c + 1, pw) == 0;
        }
    }
    return false;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"sebastian-ota\"");
        httpd_resp_sendstr(req, "нужен пароль OTA (curl -u ota:ПАРОЛЬ) или он не задан в /setup\n");
        return ESP_OK;
    }
    if (s_active) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "уже идёт прошивка\n");
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    int total = req->content_len;
    if (!part || total <= 0 || (size_t)total > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "нет слота или неверный размер");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "приём прошивки %d байт -> %s", total, part->label);
    s_active = true;
    if (s_cfg.on_progress) s_cfg.on_progress(0, false);
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    char *buf = malloc(4096);
    if (!buf) err = ESP_ERR_NO_MEM;
    int got = 0, last_pct = -1;
    bool hdr_checked = false;
    int timeouts = 0;
    while (err == ESP_OK && got < total) {
        int r = httpd_req_recv(req, buf, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;     // клиент завис > ~1 мин — бросаем
        if (r <= 0) { err = ESP_ERR_TIMEOUT; break; }
        timeouts = 0;
        if (!hdr_checked) {              // первый кусок: это вообще прошивка ESP?
            hdr_checked = true;
            if ((unsigned char)buf[0] != 0xE9) { err = ESP_ERR_INVALID_ARG; ESP_LOGE(TAG, "это не .bin приложения (нет 0xE9)"); break; }
        }
        err = esp_ota_write(h, buf, r);
        got += r;
        int pct = (int)((int64_t)got * 100 / total);
        if (pct != last_pct) {
            last_pct = pct;
            if (s_cfg.on_progress) s_cfg.on_progress(pct, false);
        }
    }
    free(buf);
    if (!buf) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = esp_ota_end(h);
    else if (h) esp_ota_abort(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA ошибка: %s", esp_err_to_name(err));
        s_active = false;
        if (s_cfg.on_progress) s_cfg.on_progress(-1, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "прошивка записана, перезагрузка");
    if (s_cfg.on_progress) s_cfg.on_progress(100, true);
    httpd_resp_sendstr(req, "OK, перезагружаюсь. Новая прошивка проверит себя ~30 с, иначе откат.\n");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

esp_err_t seb_ota_register(httpd_handle_t srv)
{
    httpd_uri_t u = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    return httpd_register_uri_handler(srv, &u);
}
