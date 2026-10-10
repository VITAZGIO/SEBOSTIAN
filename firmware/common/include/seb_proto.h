// Протокол UART голова <-> со-процессор: текстовые строки, '\n' в конце, 115200 8N1.
// Совместим со старыми Arduino-прошивками (голова v21, со-процессор v7.2):
// лишние поля в конце строки игнорируются, недостающие = значение по умолчанию.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- со-процессор -> голова ----------
typedef enum {
    CP_NONE = 0,
    CP_STATUS,      // S,t1,t2,t3,mic,bat_mV,uptime,bt,music[,mux]
    CP_TOUCH,       // T,n
    CP_PONG,        // P,n
    CP_RF_TRIG,     // R,n,code
    CP_RF_LIGHT,    // L,n,code
    CP_RF_UNK,      // U,code
    CP_RF_SENT,     // X,n,code (n = 22 — HOLD)
    CP_OTA,         // W,state,pct,ip
    CP_BAT,         // B,level,mV
    CP_GRACE,       // G,mV
    CP_FORCE_BTN,   // F
    CP_HELLO,       // HELLO ...
    CP_VERSION,     // V,версия  (новое)
    CP_CFG_REQ,     // CFG?      (новое: со-процессор просит Wi-Fi для OTA)
    CP_UNKNOWN,
} seb_cp_type_t;

typedef struct {
    seb_cp_type_t type;
    // статус
    int t1, t2, t3, mic, bat_mv;
    uint32_t uptime;
    int bt, music, mux;     // mux = -1, если поля нет
    // события
    int n;                  // номер сенсора / триггера / кода / PING
    uint32_t code;
    int ota_state, ota_pct;
    int bat_level;
    char text[40];          // ip (W), версия (V), остаток строки (HELLO)
} seb_cp_msg_t;

bool seb_proto_parse_cp(const char *line, seb_cp_msg_t *m);

// ---------- голова -> со-процессор ----------
typedef enum {
    HC_NONE = 0,
    HC_PING,        // PING n
    HC_BEEP,        // BEEP
    HC_MUSIC,       // MUSIC
    HC_LED,         // LED
    HC_RF,          // RF n (1..21)
    HC_RFHOLD,      // RFHOLD
    HC_OTA,         // OTA
    HC_SLEEP,       // SLEEP
    HC_FORCE,       // FORCE
    HC_S3B,         // S3B 0/1 (устарело, принимаем молча)
    HC_CFG,         // CFG <hex ssid> <hex pass> <hex ota> (новое)
    HC_UNKNOWN,
} seb_hc_type_t;

typedef struct {
    seb_hc_type_t type;
    uint32_t n;
    char ssid[33];
    char pass[65];
    char ota[65];
} seb_hc_msg_t;

bool seb_proto_parse_hc(const char *line, seb_hc_msg_t *m);

// Строка CFG: пароли в hex, чтобы пробелы/запятые в них ничего не ломали.
// Возвращает длину строки (без '\0') или -1, если не влезло.
int seb_proto_fmt_cfg(char *out, size_t cap, const char *ssid, const char *pass, const char *ota);

// Разбор целых через запятую: до max полей, возвращает сколько разобрано.
int seb_csv_ints(const char *s, long *out, int max);

#ifdef __cplusplus
}
#endif
