// Пины со-процессора (ESP32 D0WD-V3, 4 МБ, без PSRAM). Проверено на стенде (Arduino v7.2), ТЗ §2.3.
// НЕЛЬЗЯ: 12 (strapping, HIGH = не загрузится), 0/2, 1/3 (USB-мост CH340). Свободны: 14, 18, 21, 39 (19 и 23 — аудио-связь).
#pragma once

#define PIN_PCM_BCK     26      // I2S0 -> PCM5102A (AUX)
#define PIN_PCM_LCK     25
#define PIN_PCM_DIN     22
#define PIN_LED         32      // ARGB -> 470 Ом -> Din ленты
#define LED_COUNT       4
#define PIN_T1          36      // TTP223: HIGH = касание (34-39 только вход, без подтяжек)
#define PIN_T2          35
#define PIN_T3          34
#define PIN_MIC_BTN     4       // кнопка мика на GND, INPUT_PULLUP: LOW = нажата
#define PIN_BAT_ADC     33      // ADC1_CH5, делитель 1:2 от B+
#define PIN_LINK_RX     16      // UART2 RX <- TX головы (GPIO8)
#define PIN_LINK_TX     17      // UART2 TX -> RX головы (GPIO18)
#define LINK_UART       UART_NUM_2
// Аудио-связь с головой — отдельный быстрый UART1 (3 Мбит/с), два новых провода:
#define PIN_ALINK_TX    23      // -> голова GPIO1: Bluetooth в динамики
#define PIN_ALINK_RX    19      // <- голова GPIO46: голос/музыка головы в AUX (подтяжка ВНИЗ — strapping головы)
#define PIN_RF_RX       27      // приёмник 433 DATA (RX от 5 В — через делитель 10к/20к!)
#define PIN_RF_TX       13      // передатчик 433 DATA

// Батарея (мВ, одна банка)
#define BAT_CAL_MV      0       // поправка АЦП: (мультиметр на B+) минус (что пишет экран). Пример: 3.92 − 3.87 -> 50
#define BAT_NONE        2500    // ниже = аккума нет (стенд от USB) — защиту НЕ включаем
#define BAT_LOW         3300    // предупреждение: красные вспышки, на экране LOW BAT
#define BAT_OFF         3000    // среднее за 10 с ниже -> всё выключаем и спим (железная защита модуля — 2.5 В)
#define BAT_WAKE        3550    // проснуться только выше этого (после нагрузки аккум «отпрыгивает» до ~3.3 В)
#define SLEEP_CHECK_S   60      // во сне просыпаемся раз в минуту проверить аккум
