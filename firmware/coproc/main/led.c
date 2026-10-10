#include "led.h"
#include <string.h>
#include "board.h"
#include "led_strip_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "led";
static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_mx;
static int s_mode = 5;
static uint16_t s_hue;
static int64_t s_t_rb;
#define BRIGHT 40               // из 255, как strip.setBrightness(40)

static void show(const uint8_t *rgb)   // rgb[LED_COUNT*3]
{
    if (!s_ch) return;
    uint8_t px[LED_COUNT * 3];
    for (int i = 0; i < LED_COUNT; i++) {          // WS2812 ждёт порядок G, R, B
        px[3 * i] = rgb[3 * i + 1] * BRIGHT / 255;
        px[3 * i + 1] = rgb[3 * i] * BRIGHT / 255;
        px[3 * i + 2] = rgb[3 * i + 2] * BRIGHT / 255;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(s_ch, s_enc, px, sizeof(px), &tc);
    rmt_tx_wait_all_done(s_ch, 50);
    xSemaphoreGive(s_mx);
}

void led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[LED_COUNT * 3];
    for (int i = 0; i < LED_COUNT; i++) { rgb[3 * i] = r; rgb[3 * i + 1] = g; rgb[3 * i + 2] = b; }
    show(rgb);
}

void led_off(void) { led_fill(0, 0, 0); }

static void apply_mode(void)
{
    switch (s_mode) {
    case 0: led_off(); break;
    case 1: led_fill(255, 0, 0); break;
    case 2: led_fill(0, 255, 0); break;
    case 3: led_fill(0, 0, 255); break;
    case 4: led_fill(255, 255, 255); break;
    default: break;                         // радуга крутится в led_tick
    }
}

void led_next_mode(void)
{
    static const char *N[] = {"OFF", "RED", "GREEN", "BLUE", "WHITE", "RAINBOW"};
    s_mode = (s_mode + 1) % 6;
    ESP_LOGI(TAG, "режим %s", N[s_mode]);
    apply_mode();
}

int led_mode(void) { return s_mode; }

// оттенок 0..65535 -> RGB (как Adafruit ColorHSV) + гамма ~2.6 (как gamma32)
static void hsv(uint16_t h, uint8_t *o)
{
    uint32_t hh = ((uint32_t)h * 1530 + 32768) / 65536;
    uint8_t r, g, b;
    if (hh < 510) { b = 0; if (hh < 255) { r = 255; g = hh; } else { r = 510 - hh; g = 255; } }
    else if (hh < 1020) { r = 0; if (hh < 765) { g = 255; b = hh - 510; } else { g = 1020 - hh; b = 255; } }
    else if (hh < 1530) { g = 0; if (hh < 1275) { r = hh - 1020; b = 255; } else { r = 255; b = 1530 - hh; } }
    else { r = 255; g = b = 0; }
    uint8_t c[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        float x = c[i] / 255.0f;
        o[i] = (uint8_t)(x * x * (0.6f * x + 0.4f) * 255.0f + 0.5f);   // приближение x^2.6
    }
}

void led_tick(void)
{
    int64_t t = esp_timer_get_time() / 1000;
    if (s_mode != 5 || t - s_t_rb < 20) return;
    s_t_rb = t;
    s_hue += 512;                           // 128 шагов ≈ 2.5 с на круг
    uint8_t rgb[LED_COUNT * 3];
    for (int i = 0; i < LED_COUNT; i++) hsv(s_hue + i * 65536 / LED_COUNT, rgb + 3 * i);
    show(rgb);
}

void led_flash(uint8_t r, uint8_t g, uint8_t b, int times, int on_ms, int off_ms)
{
    for (int i = 0; i < times; i++) {
        led_fill(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        led_off();
        if (off_ms) vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
    apply_mode();
}

void led_init(void)
{
    if (s_ch) return;
    s_mx = xSemaphoreCreateMutex();
    gpio_hold_dis(PIN_LED);
    rmt_tx_channel_config_t cc = {
        .gpio_num = PIN_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&cc, &s_ch) != ESP_OK) { s_ch = NULL; return; }
    led_strip_encoder_config_t ec = {.resolution = 10 * 1000 * 1000};
    rmt_new_led_strip_encoder(&ec, &s_enc);
    rmt_enable(s_ch);
    led_off();
}

void led_hold_low_for_sleep(void)
{
    if (s_ch) {
        led_off();
        rmt_disable(s_ch);
        rmt_del_channel(s_ch);
        s_ch = NULL;
    }
    gpio_reset_pin(PIN_LED);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);
    gpio_hold_en(PIN_LED);
}
