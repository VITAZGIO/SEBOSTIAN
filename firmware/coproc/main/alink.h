// Аудио-связь с головой по отдельному UART1 (3 Мбит/с, пакеты с CRC — common/seb_alink.h):
//   отдача GPIO23 -> голова GPIO1 : Bluetooth (PCM стерео) — если голова просит BT в динамики
//   приём  GPIO19 <- голова GPIO46: звук головы (48 кГц стерео) -> микшер -> AUX
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "seb_util.h"

void cpal_init(void);
void cpal_bt_push(const uint8_t *d, uint32_t len);   // из колбэка A2DP (не блокирует)
seb_ring_t *cpal_head_ring(void);
bool cpal_duck(void);                  // голова говорит «приглуши Bluetooth» (флаг в пакете или DUCK 1)
void cpal_set_duck(bool on);           // команда DUCK от головы
void cpal_report(void);                // строка A,ok,crc,lost голове
void cpal_sleep(void);                 // перед глубоким сном
