#include "power.h"
#include "board.h"
#include "state.h"
#include "link.h"
#include "batlog.h"
#include "mp3.h"
#include "net.h"
#include "audio.h"
#include "display.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bat";
static RTC_DATA_ATTR bool s_head_bat_sleep;     // уснули из-за аккума (переживает глубокий сон)
static int64_t s_t_lowbat, s_grace_done;
static int s_last_bat;

static void out_level(int pin, int level)
{
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void sleep_now(void)
{
    out_level(PIN_SPK_MUTE, 1);                  // MAX98357A молчат
    out_level(PIN_XSMT, 0);                      // PCM5102A молчит
    out_level(PIN_LCD_BL, 0);                    // подсветка выкл
    gpio_hold_en(PIN_SPK_MUTE);                  // держать эти уровни и во сне
    gpio_hold_en(PIN_XSMT);
    gpio_hold_en(PIN_LCD_BL);
    gpio_deep_sleep_hold_en();
    // ждём, пока со-процессор замолчит (иначе его строки сразу нас разбудят)
    int64_t t0 = now_ms();
    while (now_ms() - t0 < 10000 && g_st.last_rx_ms && now_ms() - g_st.last_rx_ms < 1500) vTaskDelay(pdMS_TO_TICKS(20));
    s_head_bat_sleep = true;
    esp_sleep_enable_ext1_wakeup_io((1ULL << PIN_LINK_RX) | (1ULL << PIN_TOUCH_IRQ), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);     // подтяжки работают во сне
    rtc_gpio_pullup_en(PIN_LINK_RX);
    rtc_gpio_pulldown_dis(PIN_LINK_RX);
    rtc_gpio_pullup_en(PIN_TOUCH_IRQ);
    rtc_gpio_pulldown_dis(PIN_TOUCH_IRQ);
    ESP_LOGW(TAG, "сплю. Разбудит со-процессор (зарядка) или касание экрана.");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

void power_boot_gate(void)
{
    gpio_deep_sleep_hold_dis();                  // отпускаем пины, которые держали во сне
    gpio_hold_dis(PIN_SPK_MUTE);
    gpio_hold_dis(PIN_XSMT);
    gpio_hold_dis(PIN_LCD_BL);
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) { s_head_bat_sleep = false; return; }
    bool by_touch = esp_sleep_get_ext1_wakeup_status() & (1ULL << PIN_TOUCH_IRQ);
    rtc_gpio_deinit(PIN_LINK_RX);                // вернуть пины из режима «будильника» в обычный
    rtc_gpio_deinit(PIN_TOUCH_IRQ);
    if (!s_head_bat_sleep) return;
    ESP_LOGW(TAG, "проснулся: %s", by_touch ? "касание экрана" : "сигнал от со-процессора");
    out_level(PIN_SPK_MUTE, 1);
    out_level(PIN_XSMT, 0);
    if (by_touch) {
        g_st.bat_dead = true;
        g_st.bat_dead_mv = 0;
        display_init();
        ui_init();
        display_backlight(true);
    } else {
        out_level(PIN_LCD_BL, 0);
    }
    link_uart_setup();
    if (link_wait_alive(4000)) {                 // со-процессор в обычном режиме шлёт "S,..." 5 раз в секунду
        s_head_bat_sleep = false;
        g_st.bat_dead = false;
        ESP_LOGW(TAG, "со-процессор проснулся (зарядка) — обычный старт");
        return;
    }
    ESP_LOGW(TAG, "со-процессор спит — аккум всё ещё сел, сплю дальше");
    sleep_now();
}

void power_bat_shutdown(int mv)
{
    ESP_LOGW(TAG, "аккум сел (%.2f В) — выключаюсь", mv / 1000.0f);
    link_send("SLEEP");                          // со-процессору: понял, можешь спать
    batlog_flush();
    mp3_stop();
    audio_flush(SRC_TTS);
    audio_flush(SRC_FX);
    gpio_set_level(PIN_SPK_MUTE, 1);
    gpio_set_level(PIN_XSMT, 0);
    g_st.bat_dead_mv = mv;
    g_st.bat_dead = true;
    net_stop();
    vTaskDelay(pdMS_TO_TICKS(3000));
    sleep_now();
}

void power_grace(int mv)
{
    if (s_grace_done && now_ms() - s_grace_done < 15000) return;
    ESP_LOGW(TAG, "включили, аккум сел (%.2f В): 5 с на кнопку ВКЛЮЧИТЬ", mv / 1000.0f);
    mp3_stop();
    g_st.grace_mv = mv;
    g_st.grace_forced = false;
    g_st.grace_until = now_ms() + 5000;
    while (!g_st.grace_forced && now_ms() < g_st.grace_until) vTaskDelay(pdMS_TO_TICKS(20));
    bool forced = g_st.grace_forced;
    g_st.grace_until = 0;
    s_grace_done = now_ms();
    if (forced) {
        link_send("FORCE");
        ESP_LOGW(TAG, "FORCE — работаем от зарядки (выключусь, если аккум < 3.0 В)");
        st_flash(COL_DKGREEN, "FORCE: работаю от зарядки");
        return;
    }
    power_bat_shutdown(mv);
}

void power_tick(void)
{
    if (link_rx_ok()) s_last_bat = g_st.cp_bat;
    int b = g_st.cp_bat;
    if (link_rx_ok() && b >= BAT_NONE_MV && b < BAT_LOW_MV && now_ms() - s_t_lowbat > 60000) {
        s_t_lowbat = now_ms();
        st_flash(COL_RED, "LOW BAT %.2f В — заряди!", b / 1000.0f);
    }
    // запасной путь: "B,2" потерялся, со-процессор уже спит
    if (!link_rx_ok() && g_st.last_rx_ms && now_ms() - g_st.last_rx_ms > 30000 && s_last_bat >= BAT_NONE_MV &&
        s_last_bat < 3150)
        power_bat_shutdown(s_last_bat);
}
