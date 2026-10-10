#include "seb_logbuf.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static char *s_buf;
static size_t s_size, s_wr;          // s_wr растёт бесконечно
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static int log_vprintf(const char *fmt, va_list ap)
{
    char tmp[192];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap2);
    va_end(ap2);
    if (n > 0 && s_buf) {
        if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
        portENTER_CRITICAL_SAFE(&s_mux);
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\033') c = ' ';                 // цветовые коды не нужны в браузере
            s_buf[s_wr % s_size] = c;
            s_wr++;
        }
        portEXIT_CRITICAL_SAFE(&s_mux);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void seb_logbuf_init(size_t size, bool psram)
{
    s_buf = heap_caps_malloc(size, psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT);
    if (!s_buf) return;
    s_size = size;
    s_wr = 0;
    s_prev = esp_log_set_vprintf(log_vprintf);
}

size_t seb_logbuf_get(char *out, size_t cap)
{
    if (!s_buf || !cap) return 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t have = s_wr < s_size ? s_wr : s_size;
    if (have > cap - 1) have = cap - 1;
    size_t start = s_wr - have;
    for (size_t i = 0; i < have; i++) out[i] = s_buf[(start + i) % s_size];
    portEXIT_CRITICAL_SAFE(&s_mux);
    out[have] = 0;
    return have;
}

static esp_err_t log_get(httpd_req_t *req)
{
    size_t cap = s_size + 1;
    char *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) b = malloc(cap);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    size_t n = seb_logbuf_get(b, cap);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, b, n);
    free(b);
    return ESP_OK;
}

esp_err_t seb_logbuf_register(httpd_handle_t srv)
{
    httpd_uri_t u = {.uri = "/log", .method = HTTP_GET, .handler = log_get};
    return httpd_register_uri_handler(srv, &u);
}
