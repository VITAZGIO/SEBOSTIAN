// UART2 к голове: приём команд (PING, BEEP, MUSIC, LED, RF n, RFHOLD, OTA, SLEEP, FORCE, CFG) и отправка строк.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "board.h"
#include "cp.h"
#include "led.h"
#include "rf433.h"
#include "btaudio.h"
#include "otamode.h"
#include "seb_proto.h"
#include "seb_util.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "link";
static SemaphoreHandle_t s_mx;
static bool s_up;
volatile int64_t g_last_ping_ms;
volatile uint32_t g_pings;
volatile bool g_sleep_ack;
volatile int g_grace_reply;

void link_send(const char *fmt, ...)
{
    if (!s_up) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
    b[n++] = '\n';
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uart_write_bytes(LINK_UART, b, n);
    xSemaphoreGive(s_mx);
}

static void handle(const char *s)
{
    seb_hc_msg_t m;
    seb_proto_parse_hc(s, &m);
    switch (m.type) {
    case HC_PING:                               // PING n -> отвечаем P,n
        g_last_ping_ms = now_ms();
        g_pings++;
        link_send("P,%lu", (unsigned long)m.n);
        return;
    case HC_S3B: return;                        // устарело (было для 74HC4053)
    case HC_SLEEP: g_sleep_ack = true; g_grace_reply = 2; return;
    case HC_FORCE: g_grace_reply = 1; return;
    case HC_CFG:
        strncpy(g_cfg.ssid, m.ssid, sizeof(g_cfg.ssid) - 1);
        strncpy(g_cfg.pass, m.pass, sizeof(g_cfg.pass) - 1);
        strncpy(g_cfg.ota, m.ota, sizeof(g_cfg.ota) - 1);
        cfg_save();
        ESP_LOGI(TAG, "от головы получены настройки Wi-Fi (\"%s\") и пароль OTA", g_cfg.ssid);
        return;
    default: break;
    }
    if (g_ota_mode) return;                     // в режиме прошивки остальное не выполняем
    ESP_LOGI(TAG, "команда от головы: %s", s);
    switch (m.type) {
    case HC_BEEP: bta_beep(); break;
    case HC_MUSIC: bta_music_toggle(); break;
    case HC_LED: led_next_mode(); break;
    case HC_RF: rf433_send_light((int)m.n); break;
    case HC_RFHOLD: rf433_hold(); break;
    case HC_OTA: ota_mode_request(); break;
    default: break;
    }
}

static void link_task(void *arg)
{
    static char lbuf[200];
    seb_linebuf_t lb;
    seb_linebuf_init(&lb, lbuf, sizeof(lbuf));
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            if (seb_linebuf_put(&lb, (char)buf[i])) {
                if (lb.len && !lb.overflow) handle(lb.buf);
                seb_linebuf_reset(&lb);
            }
        }
    }
}

void link_init(void)
{
    if (s_up) return;
    s_mx = xSemaphoreCreateMutex();
    uart_config_t c = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0);
    uart_param_config(LINK_UART, &c);
    uart_set_pin(LINK_UART, PIN_LINK_TX, PIN_LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_up = true;
    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 10, NULL, 0);
}

void link_stop_for_sleep(void)
{
    if (s_up) {
        uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(100));
        s_up = false;
    }
    gpio_reset_pin(PIN_LINK_TX);
    gpio_set_direction(PIN_LINK_TX, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_LINK_TX);
}
