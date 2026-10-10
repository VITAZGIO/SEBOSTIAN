// Общее для модулей со-процессора: время, связь с головой, состояние.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---- связь с головой (link.c) ----
void link_init(void);
void link_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void link_stop_for_sleep(void);         // отпустить TX (у головы там подтяжка)
extern volatile int64_t g_last_ping_ms;
extern volatile uint32_t g_pings;
extern volatile bool g_sleep_ack;       // голова ответила SLEEP
extern volatile int g_grace_reply;      // 1 = FORCE, 2 = SLEEP (окно на севшем аккуме)

// ---- настройки (NVS) ----
typedef struct {
    char ssid[33], pass[65], ota[65];   // приходят от головы строкой CFG
} cp_cfg_t;
extern cp_cfg_t g_cfg;
void cfg_load(void);
void cfg_save(void);
bool lowflag_get(void);
void lowflag_set(bool v);

// ---- общее состояние ----
extern volatile int g_t[3];             // сенсоры (стабильные)
extern volatile int g_mic_btn;
extern volatile int g_bat_mv;           // среднее за 10 с
extern volatile bool g_ota_mode;
