// Общее состояние головы: его пишут модули (связь, звук, голос...), читает экран и /status.
// Числа — volatile (32 бита пишутся атомарно), строки — только под st_lock().
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// цвета 0xRRGGBB (как в старой прошивке TFT_*)
#define COL_WHITE   0xFFFFFF
#define COL_GREEN   0x00E000
#define COL_RED     0xFF3030
#define COL_YELLOW  0xFFE000
#define COL_CYAN    0x00E0FF
#define COL_GREY    0x808080
#define COL_ORANGE  0xFF9000
#define COL_DKGREEN 0x007B00
#define COL_NAVY    0x00007B
#define COL_DKRED   0x7B0000
#define COL_BTN     0x303030

enum { PG_HOME, PG_VOICE, PG_TRAIN, PG_AUDIO, PG_OUT, PG_SENS, PG_RADIO, PG_SYS, PG_COUNT };

typedef struct {
    // ---- связь с со-процессором ----
    volatile int64_t last_rx_ms, last_ack_ms;
    volatile uint32_t rx_lines, rx_bytes, ping_sent, last_ack;
    volatile int cp_t1, cp_t2, cp_t3, cp_mic, cp_bat, cp_bt, cp_music, cp_mux;
    volatile uint32_t cp_up;
    char cp_ver[24];
    volatile int cp_ota, cp_ota_pct;
    char cp_ip[20];
    char rf_rx[48];
    volatile int64_t rf_rx_ms;
    char rf_tx[32];

    // ---- звук ----
    volatile bool sd_ok, file_ok, mic_ok, aux_plug, spk_muted, aux_muted;
    volatile uint32_t sd_mb;
    volatile float mic_level;

    // ---- голос ----
    char v_srv[96];
    char v_state[128];
    volatile uint32_t v_state_col;
    char v_you[320];
    char v_seb[640];
    char v_heard[200];
    char v_t1[64];
    char v_t2[96];
    volatile bool voice_busy;       // рабочая задача голоса занята (запись/ответ/тест)

    // ---- слушатель «Себастьян» ----
    volatile bool ls_running, ls_paused;
    volatile int ls_state;          // 0 ждём речь, 1 пишем фразу, 2 распознаём
    volatile int ls_mode;           // 0 ждём слово, 1 ждём команду
    volatile float ls_nf, ls_lvl, ls_thr;

    // ---- эхоподавление ----
    volatile bool aec_ok;
    volatile float aec_erle_db, aec_delay_ms, aec_ref_lvl;

    // ---- TRAIN ----
    volatile int kws_pos, kws_neg, kws_bg, kws_tip;
    char kws_state[96];
    volatile uint32_t kws_state_col;
    char kws_info[96];
    char kws_big[40];
    char kws_small[96];
    volatile uint32_t kws_prompt_col;
    char kws_last[64];

    // ---- система ----
    volatile int head_ota_pct;      // -1 = не прошиваемся
    volatile uint32_t batlog_lines;
    volatile int page;

    // ---- всплывающее сообщение в шапке ----
    char flash[64];
    volatile uint32_t flash_col;
    volatile int64_t flash_until;

    // ---- полноэкранные окна ----
    volatile bool bat_dead;         // «БАТАРЕЯ СЕЛА»
    volatile int bat_dead_mv;
    volatile int64_t grace_until;   // окно «ВКЛЮЧИТЬ» (0 = нет)
    volatile int grace_mv;
    volatile bool grace_forced;
} seb_state_t;

extern seb_state_t g_st;

void st_init(void);
void st_lock(void);
void st_unlock(void);
// printf в строку состояния под замком
void st_set(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ST_SET(field, ...) st_set(g_st.field, sizeof(g_st.field), __VA_ARGS__)
void st_copy(char *out, size_t cap, const char *src);    // скопировать строку состояния под замком
void st_flash(uint32_t col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

int64_t now_ms(void);
bool link_rx_ok(void);      // ESP -> S3 живо (строки моложе 1.5 с)
bool link_tx_ok(void);      // S3 -> ESP живо (ответ на PING моложе 3 с)
