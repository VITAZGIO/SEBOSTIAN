// Bluetooth-колонка «Sebastian» (A2DP sink, Bluedroid, SSP без PIN) -> I2S0 -> PCM5102A (AUX).
// Когда Bluetooth молчит — в тот же I2S играют мелодии: писк при старте/BEEP и «Ода к радости» (MUSIC).
#pragma once
#include <stdbool.h>

void bta_init(void);            // I2S + задача вывода (звук и мелодии)
void bta_bt_start(void);        // Bluetooth-стек и A2DP
void bta_bt_stop(void);         // выключить BT и освободить память (перед OTA)
int bta_bt_state(void);         // 0 ждёт, 1 подключён, 2 играет
const char *bta_peer_name(void);
void bta_beep(void);            // мелодия-писк в AUX
void bta_music_toggle(void);    // тестовая «Ода к радости» вкл/выкл (не при BT)
bool bta_music_on(void);
// Маршруты от головы (AROUTE): BT в динамики головы / BT в AUX / звук головы в AUX
void bta_set_routes(bool bt_spk, bool bt_aux, bool head_aux);
bool bta_routes_seen(void);
int bta_rate(void);             // частота Bluetooth-потока
