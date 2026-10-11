// Звук головы: I2S1 -> 2x MAX98357A, микшер на 48 кГц.
// Источники: голос Себастьяна (TTS, обычно 24 кГц), эффекты (стуки, «дзынь»), MP3 с SD,
// Bluetooth со-процессора (по аудио-связи). Звук головы может идти и в AUX — тоже по аудио-связи.
// Каждый источник со своей частотой — микшер сам пересчитывает (ресемплер Эрмита).
// Громкость общая (шагами), но не выше потолка динамиков (хрипят >= 0.5).
// Пока говорит Себастьян или пользователь — музыка приглушается (дакинг).
// То, что реально ушло в динамики, копируется в 16 кГц моно — это «reference» для эхоподавления.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "seb_util.h"

#define AUDIO_RATE 48000

typedef enum { SRC_TTS = 0, SRC_FX = 1, SRC_COUNT } audio_src_t;

void audio_init(void);

// Отдать кусок PCM микшеру (буфер из malloc/heap_caps_malloc — микшер сам сделает free).
bool audio_push(audio_src_t src, int16_t *pcm, uint32_t frames, int ch, int rate, int wait_ms);
void audio_flush(audio_src_t src);
bool audio_src_busy(audio_src_t src);
void audio_wait_idle(audio_src_t src, int max_ms);

// Статистика ответа Себастьяна (как в Arduino v14): первый звук, конец, паузы между кусками.
void audio_tts_session(bool on);
int64_t audio_tts_first_ms(void);
int64_t audio_tts_last_ms(void);
int audio_tts_gaps(void);
int audio_tts_pieces(void);

// MP3: декодер пишет PCM в кольцо, микшер забирает.
seb_ring_t *audio_mp3_ring(void);
void audio_mp3_format(int rate, int ch);
void audio_mp3_flush(void);

// Маршруты: куда реально идёт звук головы и Bluetooth (с учётом АВТО, штекера, мьюта AUX)
typedef struct {
    bool head_spk, head_aux, bt_spk, bt_aux;
} audio_route_t;
void audio_routes(audio_route_t *r);
bool audio_head_active(void);           // звучал ли собственный звук головы последние 300 мс
bool audio_voice_active(void);          // говорит Себастьян или человек — музыку приглушаем

// Громкость и выходы
extern const float AUDIO_VOLS[];
extern const int AUDIO_NVOL;
void audio_volume_step(int dir);        // VOL+ / VOL- (не выше потолка)
void audio_apply_outputs(void);         // пересчитать мьюты после смены режима/штекера/кнопок
bool audio_spk_on(void);
bool audio_aux_on(void);
bool audio_active(void);                // голова или Bluetooth звучали в динамиках последние 300 мс
void audio_user_speaking(bool on);      // слушатель слышит человека — приглушить музыку

// Эхоподавление
seb_ring_t *audio_ref_ring(void);       // 16 кГц моно, то, что ушло в динамики
int audio_out_latency_ms(void);         // сколько звук лежит в DMA до динамиков
float audio_ref_level(void);
