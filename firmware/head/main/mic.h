// Микрофон INMP441 на I2S0 (16 кГц, 32-бит слот, левый канал) + эхоподавление ESP-SR.
// Задача mic читает кадры, считает уровень, при включённом AEC вычитает то, что играют динамики,
// и раздаёт звук двум потребителям:
//   - слушателю «Себастьян» (кольцо listen, очищенный звук),
//   - записи (кольцо rec: сырой для TRAIN или очищенный для ASK) — только пока её кто-то ждёт.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "seb_util.h"

#define MIC_RATE 16000

bool mic_init(void);
seb_ring_t *mic_listen_ring(void);
// Запись: начать (сбросить старое), читать сэмплы, закончить.
void mic_rec_begin(bool raw);
int mic_rec_read(int16_t *dst, int max, int timeout_ms);
void mic_rec_end(void);
bool mic_aec_active(void);
