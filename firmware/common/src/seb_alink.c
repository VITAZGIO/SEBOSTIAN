#include "seb_alink.h"
#include <string.h>

uint16_t seb_crc16(const uint8_t *d, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t seb_alink_pack(uint8_t *o, uint8_t type, uint8_t flags, uint16_t rate, uint16_t seq, const void *pl, uint16_t len)
{
    if (len > SEB_ALINK_MAX_PAYLOAD) len = SEB_ALINK_MAX_PAYLOAD;
    o[0] = 0xA5;
    o[1] = 0x5A;
    o[2] = type;
    o[3] = flags;
    o[4] = rate & 0xFF;
    o[5] = rate >> 8;
    o[6] = seq & 0xFF;
    o[7] = seq >> 8;
    o[8] = len & 0xFF;
    o[9] = len >> 8;
    memcpy(o + 10, pl, len);
    uint16_t c = seb_crc16(o + 2, 8 + (size_t)len, 0xFFFF);
    o[10 + len] = c & 0xFF;
    o[11 + len] = c >> 8;
    return (size_t)len + SEB_ALINK_OVERHEAD;
}

enum { S_SYNC0, S_SYNC1, S_HDR, S_PAY, S_CRC };

void seb_alink_rx_init(seb_alink_rx_t *rx) { memset(rx, 0, sizeof(*rx)); }

int seb_alink_rx_feed(seb_alink_rx_t *rx, const uint8_t *d, size_t n, seb_alink_cb_t cb, void *ctx)
{
    int pk = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        switch (rx->st) {
        case S_SYNC0:
            if (c == 0xA5) rx->st = S_SYNC1;
            else rx->resync++;
            break;
        case S_SYNC1:
            if (c == 0x5A) { rx->st = S_HDR; rx->got = 0; }
            else rx->st = (c == 0xA5) ? S_SYNC1 : S_SYNC0;
            break;
        case S_HDR:
            rx->hdr[rx->got++] = c;
            if (rx->got == 8) {
                rx->pkt.type = rx->hdr[0];
                rx->pkt.flags = rx->hdr[1];
                rx->pkt.rate = (uint16_t)(rx->hdr[2] | rx->hdr[3] << 8);
                rx->pkt.seq = (uint16_t)(rx->hdr[4] | rx->hdr[5] << 8);
                rx->pkt.len = (uint16_t)(rx->hdr[6] | rx->hdr[7] << 8);
                if (rx->pkt.len > SEB_ALINK_MAX_PAYLOAD) { rx->st = S_SYNC0; rx->crc_err++; break; }
                rx->got = 0;
                rx->st = rx->pkt.len ? S_PAY : S_CRC;
            }
            break;
        case S_PAY: {
            size_t k = n - i, left = rx->pkt.len - rx->got;
            if (k > left) k = left;
            memcpy(rx->pkt.payload + rx->got, d + i, k);
            rx->got += (uint16_t)k;
            i += k - 1;
            if (rx->got == rx->pkt.len) { rx->st = S_CRC; rx->got = 0; }
        } break;
        case S_CRC:
            rx->crc[rx->got++] = c;
            if (rx->got == 2) {
                uint16_t want = seb_crc16(rx->hdr, 8, 0xFFFF);
                want = seb_crc16(rx->pkt.payload, rx->pkt.len, want);
                if (want == (uint16_t)(rx->crc[0] | rx->crc[1] << 8)) {
                    if (rx->have_seq && (uint16_t)(rx->pkt.seq - rx->last_seq) != 1)
                        rx->lost += (uint16_t)(rx->pkt.seq - rx->last_seq - 1);
                    rx->have_seq = true;
                    rx->last_seq = rx->pkt.seq;
                    rx->ok++;
                    pk++;
                    if (cb) cb(&rx->pkt, ctx);
                } else {
                    rx->crc_err++;
                }
                rx->st = S_SYNC0;
            }
            break;
        }
    }
    return pk;
}
