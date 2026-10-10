// ARGB-лента (4 диода WS2812, GPIO32 через 470 Ом) на RMT. Яркость ~15%.
// Режимы (кнопка LED NEXT на голове): OFF, RED, GREEN, BLUE, WHITE, RAINBOW. При старте — радуга.
#pragma once
#include <stdint.h>

void led_init(void);
void led_fill(uint8_t r, uint8_t g, uint8_t b);
void led_off(void);
void led_next_mode(void);
int led_mode(void);
void led_tick(void);                    // раз в ~20 мс: крутит радугу
void led_flash(uint8_t r, uint8_t g, uint8_t b, int times, int on_ms, int off_ms);   // блокирует
void led_hold_low_for_sleep(void);      // перед глубоким сном
