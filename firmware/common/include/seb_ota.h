// OTA по HTTP + автооткат (общий для головы и со-процессора).
//
// Как работает откат: загрузчик собран с CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
// Новая прошивка после OTA стартует в состоянии «на проверке». Мы помечаем её рабочей,
// только когда поднялись Wi-Fi и HTTP-сервер и прошло SEB_OTA_STABLE_S секунд без падений.
// Не поднялись за SEB_OTA_DEADLINE_S — сами перезагружаемся, и загрузчик возвращает старую.
// Если новая прошивка падает (panic/WDT) до пометки — тоже откат при следующей загрузке.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEB_OTA_STABLE_S    30
#define SEB_OTA_DEADLINE_S  120

typedef struct {
    // пароль OTA (Basic auth, любой логин; или заголовок X-OTA-Password). Пустой = OTA запрещена.
    const char *(*get_password)(void);
    // сеть и веб живы? (для пометки прошивки рабочей)
    bool (*net_ok)(void);
    // прогресс: pct 0..100, -1 = ошибка/отмена; done = всё записано, сейчас перезагрузка
    void (*on_progress)(int pct, bool done);
} seb_ota_cfg_t;

// Вызвать как можно раньше в app_main: запоминает, на проверке ли прошивка, запускает сторожа.
void seb_ota_guard_start(const seb_ota_cfg_t *cfg);
// Прошивка на проверке (ещё не помечена рабочей)?
bool seb_ota_pending(void);
// Зарегистрировать POST /ota на сервере.
esp_err_t seb_ota_register(httpd_handle_t srv);
// Идёт приём прошивки?
bool seb_ota_active(void);
// Описание текущего слота для /status: "ota_0 (valid)".
void seb_ota_describe(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
