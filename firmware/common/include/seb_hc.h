// Мини HTTP-клиент на сокетах (как в Arduino v14-v21, проверен с сервером Себастьяна).
// Понимает ответ с Content-Length, chunked и «до закрытия соединения» (SSE).
// Чтение кусками по 0.3 с — можно прервать (abort) и считать свои таймауты.
// Работает и на ESP (lwIP), и на ПК (для тестов: firmware/common/test).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;
    bool chunked, eof;
    long left;              // осталось в текущем chunk
    int64_t clen, got;      // Content-Length (-1 нет) и сколько тела прочитано
    uint8_t buf[2048];
    int pos, len;
    char hl[24];            // строка размера chunk (собирается по байту между вызовами)
    int hl_n;
    volatile bool *abort;   // если не NULL и *abort — ожидания прерываются
} seb_hc_t;

// Соединиться и отправить заголовки запроса (clen < 0 — без тела).
bool seb_hc_request(seb_hc_t *h, const char *host, int port, const char *method, const char *path,
                    const char *ctype, const char *accept, int clen, int connect_timeout_ms);
bool seb_hc_send(seb_hc_t *h, const void *d, int n);
// Статус и заголовки: HTTP-код или -1 (таймаут wait_ms / обрыв / прервали).
int seb_hc_head(seb_hc_t *h, int wait_ms);
// Тело: >0 байт, 0 конец, -1 ошибка, -2 «пока нет данных» (ждём дальше).
int seb_hc_read(seb_hc_t *h, char *o, int n);
// Всё тело в buf (до cap-1), ждать не дольше idle_ms без данных.
int seb_hc_read_body(seb_hc_t *h, char *buf, int cap, int idle_ms);
void seb_hc_close(seb_hc_t *h);

#ifdef __cplusplus
}
#endif
