// Аудио-связь между платами по отдельному быстрому UART (3 Мбит/с, в каждую сторону свой провод):
//   со-процессор GPIO23 -> голова GPIO1   : Bluetooth-звук в динамики
//   голова GPIO46 -> со-процессор GPIO19  : голос/музыка головы в AUX
// Пакет: A5 5A | type | flags | rate(2) | seq(2) | len(2) | payload(len) | crc16(2)
// payload — PCM 16 бит стерео {L,R}. CRC16-CCITT по всему после синхробайтов.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEB_ALINK_BAUD        3000000
#define SEB_ALINK_MAX_PAYLOAD 1024             // 256 стерео-кадров
#define SEB_ALINK_OVERHEAD    12
#define SEB_ALINK_T_PCM       1
#define SEB_ALINK_F_DUCK      0x01             // голова: «говорит Себастьян / человек» — приглуши Bluetooth

typedef struct {
    uint8_t type, flags;
    uint16_t rate, seq, len;
    uint8_t payload[SEB_ALINK_MAX_PAYLOAD];
} seb_apkt_t;

typedef void (*seb_alink_cb_t)(const seb_apkt_t *p, void *ctx);

typedef struct {
    int st;                 // состояние разбора
    uint16_t need, got;
    uint8_t hdr[8];
    seb_apkt_t pkt;
    uint8_t crc[2];
    bool have_seq;
    uint16_t last_seq;
    uint32_t ok, crc_err, lost, resync;
} seb_alink_rx_t;

uint16_t seb_crc16(const uint8_t *d, size_t n, uint16_t crc);
// Собрать пакет в out (нужно len + 12 байт). Возвращает размер.
size_t seb_alink_pack(uint8_t *out, uint8_t type, uint8_t flags, uint16_t rate, uint16_t seq, const void *pl, uint16_t len);
void seb_alink_rx_init(seb_alink_rx_t *rx);
// Скормить байты; на каждый целый пакет с верной CRC — cb. Возвращает число пакетов.
int seb_alink_rx_feed(seb_alink_rx_t *rx, const uint8_t *d, size_t n, seb_alink_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
