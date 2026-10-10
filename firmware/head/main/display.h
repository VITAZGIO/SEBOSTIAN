// Дисплей ST7789 (esp_lcd) + LVGL 9 + тач XPT2046 на одной шине SPI2.
// Все вызовы LVGL — только под disp_lock()/disp_unlock() (или из колбэков LVGL).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);
bool disp_lock(int timeout_ms);
void disp_unlock(void);
void display_backlight(bool on);
// Время последнего касания экрана (мс аптайма) и «палец сейчас на экране».
int64_t touch_last_ms(void);
bool touch_down_now(void);
// Снимок экрана в BMP (для /screenshot). Возвращает буфер (free) и длину.
uint8_t *display_snapshot_bmp(size_t *len);
