// Режим прошивки по Wi-Fi со-процессора (Bluetooth и Wi-Fi вместе на ESP32 = заикания, поэтому отдельно):
// команда OTA с головы -> BT выкл (память освобождается) -> Wi-Fi -> HTTP POST /ota (пароль OTA),
// статус голове "W,state,pct,ip" раз в 0.5 с; ждём 5 мин, ошибка/таймаут -> перезагрузка в обычный режим.
// Этот же режим включается сам после OTA (проверка новой прошивки) и после 3 падений подряд.
#pragma once
#include <stdbool.h>

#define COPROC_HOSTNAME "sebastian-coproc"

void ota_mode_request(void);        // из задачи link (по команде OTA)
void ota_mode_run(bool verify);     // выполняется в главной задаче, не возвращается
bool ota_mode_requested(void);
