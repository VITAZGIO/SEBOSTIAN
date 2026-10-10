// Причина старта, счётчики перезагрузок, безопасный режим.
// Безопасный режим: 3 аварийных перезапуска подряд (panic/WDT) в первые 60 с работы ->
// стартуют только Wi-Fi, веб и OTA (экран/звук/голос не запускаются), чтобы можно было
// залить исправленную прошивку по воздуху. Сбрасывается после 60 с работы или выключением питания.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_system.h"

void sysmon_init(void);
esp_reset_reason_t sysmon_reason(void);
const char *sysmon_reason_name(void);
bool sysmon_bad_reset(void);        // прошлый старт аварийный (brownout/panic/wdt)
uint32_t sysmon_boot_count(void);
bool sysmon_safe_mode(void);
void sysmon_tick(void);             // раз в секунду: через 60 с сбросить счётчик аварий
