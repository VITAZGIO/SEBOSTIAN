#include "sysmon.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sys";
static RTC_NOINIT_ATTR uint32_t s_boot_count;
static RTC_NOINIT_ATTR uint32_t s_crash_count;
static RTC_NOINIT_ATTR uint32_t s_magic;
static esp_reset_reason_t s_reason;
static bool s_safe;

void sysmon_init(void)
{
    s_reason = esp_reset_reason();
    if (s_reason == ESP_RST_POWERON || s_magic != 0x5EB0A57) {
        s_boot_count = 0;
        s_crash_count = 0;
        s_magic = 0x5EB0A57;
    }
    s_boot_count++;
    bool crash = s_reason == ESP_RST_PANIC || s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_TASK_WDT ||
                 s_reason == ESP_RST_WDT;
    if (crash) s_crash_count++;
    if (s_reason == ESP_RST_SW) s_crash_count = 0;      // перезагрузка кнопкой или после OTA — с чистого листа
    s_safe = s_crash_count >= 3;
    ESP_LOGW(TAG, "причина старта: %s, перезапуск #%lu, аварий подряд %lu%s", sysmon_reason_name(),
             (unsigned long)s_boot_count, (unsigned long)s_crash_count, s_safe ? " -> БЕЗОПАСНЫЙ РЕЖИМ (только Wi-Fi+OTA)" : "");
}

esp_reset_reason_t sysmon_reason(void) { return s_reason; }

const char *sysmon_reason_name(void)
{
    switch (s_reason) {
    case ESP_RST_POWERON: return "POWER ON";
    case ESP_RST_SW: return "SOFT";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_EXT: return "RESET BTN";
    case ESP_RST_DEEPSLEEP: return "SLEEP WAKE";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
    }
}

bool sysmon_bad_reset(void)
{
    return s_reason == ESP_RST_BROWNOUT || s_reason == ESP_RST_PANIC || s_reason == ESP_RST_INT_WDT ||
           s_reason == ESP_RST_TASK_WDT || s_reason == ESP_RST_WDT;
}

uint32_t sysmon_boot_count(void) { return s_boot_count; }
bool sysmon_safe_mode(void) { return s_safe; }

void sysmon_tick(void)
{
    if (s_crash_count && esp_timer_get_time() > 60LL * 1000000) {
        s_crash_count = 0;
    }
}
