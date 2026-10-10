#include "seb_hc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_timer.h"
static int64_t ms(void) { return esp_timer_get_time() / 1000; }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
static int64_t ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif

static bool aborted(const seb_hc_t *h) { return h->abort && *h->abort; }

static bool hc_connect(seb_hc_t *h, const char *host, int port, int timeout_ms)
{
    volatile bool *ab = h->abort;
    memset(h, 0, sizeof(*h));
    h->abort = ab;
    h->fd = -1;
    h->clen = -1;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return false;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return false; }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); return false; }
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { close(fd); return false; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval rt = {.tv_sec = 0, .tv_usec = 300000};     // чтение кусками по 0.3 с — чтобы видеть прерывание
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
    struct timeval st = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
    h->fd = fd;
    return true;
}

void seb_hc_close(seb_hc_t *h)
{
    if (h->fd >= 0) {
        shutdown(h->fd, SHUT_RDWR);
        close(h->fd);
    }
    h->fd = -1;
}

bool seb_hc_send(seb_hc_t *h, const void *d, int n)
{
    const char *p = d;
    while (n > 0) {
        int w = (int)send(h->fd, p, n > 4096 ? 4096 : n, 0);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

// сырое чтение: >0 байт, 0 закрыто, -1 ошибка, -2 таймаут (ждём дальше)
static int hc_raw(seb_hc_t *h, uint8_t *o, int n)
{
    if (h->pos < h->len) {
        int k = h->len - h->pos < n ? h->len - h->pos : n;
        memcpy(o, h->buf + h->pos, k);
        h->pos += k;
        return k;
    }
    int r = (int)recv(h->fd, h->buf, sizeof(h->buf), 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
    if (r == 0) return 0;
    h->pos = 0;
    h->len = r;
    return hc_raw(h, o, n);
}

static int hc_byte(seb_hc_t *h, int wait_ms)
{
    uint8_t c;
    int64_t t0 = ms();
    for (;;) {
        int r = hc_raw(h, &c, 1);
        if (r == 1) return c;
        if (r != -2 || aborted(h) || ms() - t0 > wait_ms) return -1;
    }
}

static int hc_line(seb_hc_t *h, char *o, int cap, int wait_ms)
{
    int n = 0, c;
    while ((c = hc_byte(h, wait_ms)) >= 0 && c != '\n') {
        if (c != '\r' && n < cap - 1) o[n++] = (char)c;
    }
    o[n] = 0;
    return (c < 0 && n == 0) ? -1 : n;
}

int seb_hc_head(seb_hc_t *h, int wait_ms)
{
    char l[256];
    if (hc_line(h, l, sizeof(l), wait_ms) < 0 || strncmp(l, "HTTP/1.", 7) != 0) return -1;
    int code = atoi(l + 9);
    while (hc_line(h, l, sizeof(l), 5000) > 0) {
        for (char *p = l; *p && *p != ':'; p++) *p = (char)tolower((unsigned char)*p);
        if (!strncmp(l, "transfer-encoding:", 18) && strstr(l, "chunked")) h->chunked = true;
        if (!strncmp(l, "content-length:", 15)) h->clen = atoll(l + 15);
    }
    return code;
}

int seb_hc_read(seb_hc_t *h, char *o, int n)
{
    if (h->eof) return 0;
    if (h->chunked) {
        while (h->left == 0) {                  // размер следующего куска: "1a2b\r\n" (перед ним \r\n прошлого)
            uint8_t c;
            int r = hc_raw(h, &c, 1);
            if (r == -2) return -2;             // сервер думает — ждём дальше
            if (r <= 0) { h->eof = true; return r == 0 ? 0 : -1; }
            if (c == '\n') {
                h->hl[h->hl_n] = 0;
                int had = h->hl_n;
                h->hl_n = 0;
                if (!had) continue;
                h->left = strtol(h->hl, NULL, 16);
                if (h->left == 0) { h->eof = true; return 0; }
            } else if (c != '\r' && h->hl_n < (int)sizeof(h->hl) - 1) {
                h->hl[h->hl_n++] = (char)c;
            }
        }
        int r = hc_raw(h, (uint8_t *)o, n < h->left ? n : (int)h->left);
        if (r > 0) h->left -= r;
        if (r == 0) { h->eof = true; return 0; }
        return r;
    }
    if (h->clen >= 0 && h->got >= h->clen) { h->eof = true; return 0; }
    int want = n;
    if (h->clen >= 0 && h->clen - h->got < want) want = (int)(h->clen - h->got);
    int r = hc_raw(h, (uint8_t *)o, want);
    if (r > 0) h->got += r;
    if (r == 0) h->eof = true;
    return r;
}

int seb_hc_read_body(seb_hc_t *h, char *buf, int cap, int idle_ms)
{
    int n = 0;
    int64_t t0 = ms();
    while (n < cap - 1) {
        int r = seb_hc_read(h, buf + n, cap - 1 - n);
        if (r > 0) { n += r; t0 = ms(); continue; }
        if (r == -2 && ms() - t0 < idle_ms && !aborted(h)) continue;
        break;
    }
    buf[n] = 0;
    return n;
}

bool seb_hc_request(seb_hc_t *h, const char *host, int port, const char *method, const char *path,
                    const char *ctype, const char *accept, int clen, int connect_timeout_ms)
{
    if (!hc_connect(h, host, port, connect_timeout_ms)) return false;
    char req[480];
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:%d\r\n", method, path, host, port);
    if (ctype) n += snprintf(req + n, sizeof(req) - n, "Content-Type: %s\r\n", ctype);
    if (clen >= 0) n += snprintf(req + n, sizeof(req) - n, "Content-Length: %d\r\n", clen);
    n += snprintf(req + n, sizeof(req) - n, "Accept: %s\r\nConnection: close\r\n\r\n", accept ? accept : "*/*");
    if (!seb_hc_send(h, req, n)) { seb_hc_close(h); return false; }
    return true;
}
