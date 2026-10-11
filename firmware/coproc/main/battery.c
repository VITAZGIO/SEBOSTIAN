#include <stdbool.h>
#include "battery.h"
#include "board.h"
#include "cp.h"
#include "led.h"
#include "rf433.h"
#include "alink.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static int s_ring[10], s_n, s_i;
static int64_t s_t_warn;

void bat_init(void)
{
    if (s_adc) return;
    adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&uc, &s_adc);
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_5, &cc);          // GPIO33 = ADC1_CH5
    adc_cali_line_fitting_config_t lc = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    s_cali_ok = adc_cali_create_scheme_line_fitting(&lc, &s_cali) == ESP_OK;
    if (!s_cali_ok) ESP_LOGW(TAG, "калибровки АЦП нет в eFuse — точность хуже, подстрой BAT_CAL_MV");
}

int bat_read_now(void)
{
    bat_init();
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, ADC_CHANNEL_5, &raw);
        if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        sum += mv;
    }
    int v = sum / 16 * 2;                         // делитель 1:2
    return v < BAT_NONE ? v : v + BAT_CAL_MV;
}

static void hold_low(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
}

void bat_go_sleep(bool announce)
{
    if (announce) {
        ESP_LOGW(TAG, "аккум сел (%.2f В в среднем) — голове команда спать, потом сплю сам", g_bat_mv / 1000.0f);
        rf433_rx_enable(false);
        led_fill(255, 0, 0);
        // шлём голове "B,2" каждые 0.3 с, пока не ответит "SLEEP" (голова могла быть занята разговором), макс 20 с
        g_sleep_ack = false;
        int64_t t0 = now_ms();
        while (!g_sleep_ack && now_ms() - t0 < 20000) {
            link_send("B,2,%d", g_bat_mv);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        ESP_LOGW(TAG, "%s", g_sleep_ack ? "голова ответила SLEEP" : "голова молчит — сплю сам");
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    rf433_rx_enable(false);
    led_hold_low_for_sleep();                     // лента не ловит мусор
    cpal_sleep();                                 // аудио-связь: TX отпускаем, RX держим вниз (strapping головы)
    link_stop_for_sleep();                        // TX отпускаем: у головы там подтяжка, лишний раз не будим
    hold_low(PIN_RF_TX);                          // передатчик 433 молчит
    gpio_deep_sleep_hold_en();
    lowflag_set(true);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_CHECK_S * 1000000ULL);
    esp_sleep_enable_ext0_wakeup(PIN_T2, 1);      // сенсор 2 (play) — проверить сразу
    ESP_LOGW(TAG, "сплю. Просыпаюсь раз в минуту или по сенсору 2.");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

// Включили тумблером на севшем аккуме: 12 с шлём голове "G,mV" и ждём FORCE / SLEEP / сенсор 2
static void grace_window(int v)
{
    led_init();
    ESP_LOGW(TAG, "включили на севшем аккуме: жду кнопку на экране или сенсор 2 (до 12 с)");
    g_grace_reply = 0;
    bool blink = false;
    int64_t t0 = now_ms(), tg = 0;
    int res = 0;
    while (!res && now_ms() - t0 < 12000) {
        if (now_ms() - tg > 300) {
            tg = now_ms();
            link_send("G,%d", v);
            blink = !blink;
            if (blink) led_fill(255, 90, 0);      // оранжевое мигание = ждём решения
            else led_off();
        }
        if (g_grace_reply) res = g_grace_reply;
        if (gpio_get_level(PIN_T2) == 1) { res = 1; link_send("F"); }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    led_off();
    if (res == 1) {
        lowflag_set(false);
        ESP_LOGW(TAG, "FORCE — работаем (выключусь, если среднее < 3.0 В)");
        return;
    }
    ESP_LOGW(TAG, "%s", res == 2 ? "голова: спать" : "никто не нажал — сплю");
    bat_go_sleep(false);
}

void bat_boot_gate(void)
{
    gpio_deep_sleep_hold_dis();                   // отпускаем пины, которые держали во сне
    gpio_hold_dis(PIN_RF_TX);
    gpio_hold_dis(PIN_LED);
    esp_sleep_wakeup_cause_t w = esp_sleep_get_wakeup_cause();
    if (!lowflag_get()) return;                   // аккум в порядке — обычный старт
    gpio_config_t t2 = {.pin_bit_mask = 1ULL << PIN_T2, .mode = GPIO_MODE_INPUT};
    gpio_config(&t2);
    int v = bat_read_now();
    bool from_sleep = w == ESP_SLEEP_WAKEUP_TIMER || w == ESP_SLEEP_WAKEUP_EXT0;
    ESP_LOGW(TAG, "старт с флагом «сел» (%s): %.2f В",
             w == ESP_SLEEP_WAKEUP_EXT0 ? "сенсор 2" : w == ESP_SLEEP_WAKEUP_TIMER ? "таймер" : "включили питание", v / 1000.0f);
    if (v >= BAT_WAKE || v < BAT_NONE) {          // зарядился (или аккум отключён, питание от USB)
        lowflag_set(false);
        ESP_LOGW(TAG, "заряжается — обычный старт, будим голову");
        return;
    }
    if (!from_sleep) { grace_window(v); return; } // включили тумблером — окно на кнопку
    if (w == ESP_SLEEP_WAKEUP_EXT0) {             // тронули сенсор — показать, что сел
        led_init();
        led_flash(255, 0, 0, 3, 150, 150);
        int64_t t0 = now_ms();
        while (gpio_get_level(PIN_T2) == 1 && now_ms() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(20));
    }
    bat_go_sleep(false);
}

void bat_tick(void)
{
    s_ring[s_i] = bat_read_now();
    s_i = (s_i + 1) % 10;
    if (s_n < 10) s_n++;
    long s = 0;
    for (int i = 0; i < s_n; i++) s += s_ring[i];
    g_bat_mv = (int)(s / s_n);
    if (s_n < 10 || now_ms() < 15000) return;    // нужно 10 с замеров после старта
    if (g_bat_mv < BAT_NONE) return;              // аккума нет (стенд от USB)
    if (g_bat_mv < BAT_OFF) bat_go_sleep(true);
    if (g_bat_mv < BAT_LOW && now_ms() - s_t_warn > 30000) {
        s_t_warn = now_ms();
        ESP_LOGW(TAG, "мало заряда: %.2f В — поставь на зарядку", g_bat_mv / 1000.0f);
        led_flash(255, 0, 0, 2, 120, 120);
    }
}
