// Копия лога (ESP_LOGx) в кольцевой буфер в памяти — чтобы читать по Wi-Fi (/log),
// когда колонка собрана и кабеля нет. В UART лог идёт как обычно.
#pragma once
#include <stddef.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void seb_logbuf_init(size_t size, bool psram);
// Скопировать последние байты лога (до cap-1), вернуть длину.
size_t seb_logbuf_get(char *out, size_t cap);
// GET /log — текст лога.
esp_err_t seb_logbuf_register(httpd_handle_t srv);

#ifdef __cplusplus
}
#endif
