// Настройки головы в NVS (переживают выключение и перепрошивку).
// Wi-Fi/пароль OTA/сервер вводятся через страницу http://<IP>/setup или точку доступа «Sebastian-setup».
// Если при сборке есть main/secrets.h — из него берутся значения по умолчанию.
#pragma once
#include <stdbool.h>

// Куда играет источник: динамики / AUX / оба / АВТО (штекер AUX вставлен -> AUX, нет -> динамики).
// Без штекера AUX физически молчит, поэтому «AUX» и «ОБА» без штекера тоже идут в динамики.
enum { OUT_SPK = 0, OUT_AUX = 1, OUT_BOTH = 2, OUT_AUTO = 3 };

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ota_pass[65];
    char srv_host[64];
    int srv_port;
    int route_head;         // голос Себастьяна, MP3, звуки головы: OUT_*
    int route_bt;           // Bluetooth со-процессора: OUT_*
    float volume;           // 0..1, шагами
    float spk_ceil;         // потолок громкости динамиков (хрип >= 0.5), по умолчанию 0.35
    bool wake_on;           // слушать «Себастьян»
    bool aec_on;            // эхоподавление (слушать во время своей речи/музыки)
    bool ns_on;             // шумодав WebRTC (по умолчанию выкл — может мешать Whisper)
    int aec_lead_ms;        // насколько reference опережает эхо (подстройка AEC)
} seb_settings_t;

extern seb_settings_t g_set;

void settings_load(void);
void settings_save(void);          // пишет только изменившиеся ключи
bool settings_have_wifi(void);
