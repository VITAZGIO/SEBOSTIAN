// WAV, tar, процент батареи.
#include "seb_util.h"
#include <string.h>
#include <stdio.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool seb_wav_parse(const uint8_t *w, size_t n, int def_rate, seb_wav_info_t *info)
{
    info->rate = def_rate;
    info->channels = 1;
    info->bits = 16;
    info->data = w;
    info->data_len = n;
    if (n > 12 && !memcmp(w, "RIFF", 4) && !memcmp(w + 8, "WAVE", 4)) {
        size_t p = 12;
        info->data = NULL;
        while (p + 8 <= n) {
            uint32_t sz = rd32(w + p + 4);
            if (!memcmp(w + p, "fmt ", 4) && p + 24 <= n) {
                info->channels = rd16(w + p + 10);
                info->rate = (int)rd32(w + p + 12);
                info->bits = rd16(w + p + 22);
            } else if (!memcmp(w + p, "data", 4)) {
                info->data = w + p + 8;
                size_t left = n - p - 8;
                info->data_len = sz < left ? sz : left;   // поток мог обрезать размер
                break;
            }
            p += 8 + sz + (sz & 1);
        }
        if (!info->data) return false;
    }
    return info->bits == 16 && (info->channels == 1 || info->channels == 2) && info->rate > 0;
}

void seb_wav_header(uint8_t h[44], int rate, uint32_t data_bytes)
{
    uint32_t riff = 36 + data_bytes, sr = (uint32_t)rate, br = (uint32_t)rate * 2;
    static const uint8_t T[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    memcpy(h, T, 44);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &sr, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 40, &data_bytes, 4);
}

int seb_bat_percent(int mv)
{
    static const int V[] = {3000, 3500, 3700, 3800, 3900, 4000, 4100, 4200};
    static const int P[] = {0, 8, 35, 55, 70, 82, 92, 100};
    if (mv <= V[0]) return 0;
    if (mv >= V[7]) return 100;
    for (int i = 1; i < 8; i++)
        if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
    return 100;
}

void seb_tar_header(uint8_t h[512], const char *name, uint32_t size, uint32_t mtime)
{
    memset(h, 0, 512);
    strncpy((char *)h, name, 99);
    memcpy(h + 100, "0000644", 7);
    memcpy(h + 108, "0000000", 7);
    memcpy(h + 116, "0000000", 7);
    snprintf((char *)h + 124, 12, "%011o", (unsigned)size);
    snprintf((char *)h + 136, 12, "%011o", (unsigned)mtime);
    h[156] = '0';
    memcpy(h + 257, "ustar", 5);
    h[263] = '0';
    h[264] = '0';
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
}
