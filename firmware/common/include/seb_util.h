// Мелкие «чистые» модули без железа — проверяются юнит-тестами на ПК (firmware/common/test).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- base64 -> байты на месте (выход короче входа). '=' и мусор пропускаются ----------
size_t seb_b64_decode_inplace(char *s, size_t n);

// ---------- мини-JSON (без библиотек, ищет ключ в плоском объекте) ----------
// Указатель на начало значения ключа key или NULL.
const char *seb_json_val(const char *s, const char *key);
// Строка по ключу -> UTF-8 (понимает \n \" \\ \uXXXX). false, если ключа нет или не строка.
bool seb_json_str(const char *s, const char *key, char *out, size_t cap);
bool seb_json_num(const char *s, const char *key, float *v);
// Экранирование строки для JSON (кавычки, слэш, управляющие символы -> пробел).
size_t seb_json_escape(const char *in, char *out, size_t cap);

// ---------- WAV ----------
typedef struct {
    int rate, channels, bits;
    const uint8_t *data;
    size_t data_len;
} seb_wav_info_t;
// Разбор RIFF/WAVE. Если заголовка нет — считаем сырым PCM 16 бит моно def_rate.
bool seb_wav_parse(const uint8_t *w, size_t n, int def_rate, seb_wav_info_t *info);
// 44-байтный заголовок PCM 16 бит моно.
void seb_wav_header(uint8_t h[44], int rate, uint32_t data_bytes);

// ---------- батарея ----------
int seb_bat_percent(int mv);        // таблица из ТЗ §3.3

// ---------- tar ----------
void seb_tar_header(uint8_t h[512], const char *name, uint32_t size, uint32_t mtime);

// ---------- построчный приёмник (UART, SSE) ----------
typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow;      // текущая строка не влезла (хвост отброшен)
} seb_linebuf_t;
void seb_linebuf_init(seb_linebuf_t *lb, char *buf, size_t cap);
// Кладёт байт; true = строка готова (lb->buf, без \r\n, с '\0'). После обработки — seb_linebuf_reset.
bool seb_linebuf_put(seb_linebuf_t *lb, char c);
void seb_linebuf_reset(seb_linebuf_t *lb);

// ---------- кириллица -> латиница (для логов и поиска слова) ----------
size_t seb_translit(const char *utf8, char *out, size_t cap);

// ---------- слово «Себастьян» ----------
// true, если в фразе есть слово-ключ (себас/себос/севас/себус/сабас/себес — в любом регистре,
// кириллицей или латиницей). cmd = остаток фразы без ключа и знаков по краям (UTF-8),
// пусто, если букв в остатке < 2.
bool seb_wake_split(const char *text, char *cmd, size_t cap);

// ---------- кольцевой буфер int16 (один писатель, один читатель, без блокировок) ----------
typedef struct {
    int16_t *buf;
    uint32_t size;               // в сэмплах
    volatile uint32_t wr, rd;    // счётчики (растут бесконечно, индекс = % size)
} seb_ring_t;
void seb_ring_init(seb_ring_t *r, int16_t *buf, uint32_t size);
uint32_t seb_ring_fill(const seb_ring_t *r);
uint32_t seb_ring_space(const seb_ring_t *r);
uint32_t seb_ring_write(seb_ring_t *r, const int16_t *x, uint32_t n);    // сколько записал
uint32_t seb_ring_read(seb_ring_t *r, int16_t *x, uint32_t n);           // сколько прочитал
// Непрерывный кусок для чтения без копирования (может быть короче fill — до конца массива).
uint32_t seb_ring_peek(const seb_ring_t *r, const int16_t **p);
void seb_ring_consume(seb_ring_t *r, uint32_t n);
void seb_ring_clear(seb_ring_t *r);   // вызывать только читателю

// ---------- ресемплер (4-точечная интерполяция Эрмита), 1 или 2 канала ----------
typedef struct {
    int ch;
    uint32_t in_rate, out_rate;
    double step, frac;
    float h[4][2];          // история: x[-1], x0, x1, x2
    bool primed;
} seb_rs_t;
void seb_rs_init(seb_rs_t *rs, int ch, uint32_t in_rate, uint32_t out_rate);
// Делает до out_frames стерео-кадров (out — всегда стерео int16 L,R), берёт из in сколько нужно.
// *in_used = сколько входных кадров съел. Возвращает сколько выходных кадров сделал.
int seb_rs_process(seb_rs_t *rs, const int16_t *in, int in_frames, int *in_used,
                   float *out_lr, int out_frames);

// ---------- мягкий лимитер (стерео float, -1..1) ----------
typedef struct {
    float thr;              // порог (доля полной шкалы), например 0.7
    float env;              // огибающая
    float att, rel;         // коэффициенты
    float gain;             // последний коэффициент усиления (для отладки)
} seb_lim_t;
void seb_lim_init(seb_lim_t *l, float thr, int rate);
void seb_lim_process(seb_lim_t *l, float *lr, int frames);

// ---------- детектор речи по громкости (VAD) из Arduino v15-v17 ----------
typedef struct {
    float vad_min;          // ниже — никогда не речь (40)
    float start_k;          // начало: громче шума в k раз (3.0) ...
    int start_frames;       // ... столько кадров подряд (3 по 20 мс)
    float end_k;            // конец: тише шума ×k (2.0) ...
    int sil_ms;             // ... столько мс (700)
    int min_speech_ms;      // короче — щелчок (400)
} seb_vad_cfg_t;

typedef enum {
    SEB_VAD_NONE = 0,
    SEB_VAD_START,          // началась фраза (взять предзапись)
    SEB_VAD_DONE,           // фраза кончилась нормально
    SEB_VAD_SHORT,          // это был щелчок — выбросить
    SEB_VAD_OVERFLOW,       // фраза слишком длинная — это шум, порог поднят
} seb_vad_ev_t;

typedef struct {
    seb_vad_cfg_t cfg;
    float nf;               // шум (адаптивный)
    float lvl;              // текущий уровень (RMS после ФВЧ)
    float hp, prev;         // ФВЧ ~120 Гц
    int above, sil_ms, sp_ms;
    int state;              // 0 ждём, 1 фраза
    int tail_ms;            // для DONE: сколько мс тишины в конце фразы
} seb_vad_t;
void seb_vad_default_cfg(seb_vad_cfg_t *c);
void seb_vad_init(seb_vad_t *v, const seb_vad_cfg_t *cfg);
// Кадр 16 кГц (обычно 20 мс). phrase_full = буфер фразы уже полон.
seb_vad_ev_t seb_vad_frame(seb_vad_t *v, const int16_t *x, int n, bool phrase_full);
float seb_vad_threshold(const seb_vad_t *v);

// ---------- радио 433, протокол 1 RCSwitch (приём по фронтам, передача) ----------
#define SEB_RC_MAX_CHANGES 67
typedef struct {
    uint32_t timings[SEB_RC_MAX_CHANGES];
    unsigned change_count, repeat_count;
} seb_rc_rx_t;
typedef struct {
    uint32_t code;
    int bits;
    int delay_us;
} seb_rc_code_t;
void seb_rc_rx_init(seb_rc_rx_t *rx);
// Длительность между соседними фронтами (мкс). true = принят код (протокол 1).
bool seb_rc_rx_edge(seb_rc_rx_t *rx, uint32_t duration_us, seb_rc_code_t *out);
// Передача: пары (уровень HIGH мкс, уровень LOW мкс) — bits символов данных + 1 синхро. Возвращает число пар.
int seb_rc_tx_encode(uint32_t code, int bits, int pulse_us, uint16_t hi_us[], uint16_t lo_us[], int max_pairs);

#ifdef __cplusplus
}
#endif
