// Аудио-связь с со-процессором по отдельному UART2 (3 Мбит/с, пакеты с CRC — common/seb_alink.h):
//   приём  GPIO1  <- со-процессор GPIO23 : Bluetooth (PCM стерео, обычно 44.1 кГц) -> микшер -> динамики
//   отдача GPIO46 -> со-процессор GPIO19 : звук головы (48 кГц стерео) -> AUX
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "seb_util.h"

void alink_init(void);
seb_ring_t *alink_bt_ring(void);        // int16 стерео от со-процессора
int alink_bt_rate(void);                // частота Bluetooth-потока (0 — ещё не было)
int64_t alink_bt_last_ms(void);         // когда пришёл последний пакет
void alink_aux_push(const int16_t *lr, int frames);   // звук головы в AUX (из микшера)
void alink_set_duck(bool on);           // флаг «говорят» — со-процессор приглушит Bluetooth в AUX
