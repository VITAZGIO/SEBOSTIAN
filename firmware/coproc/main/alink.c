#include "alink.h"
#include <string.h>
#include "board.h"
#include "cp.h"
#include "btaudio.h"
#include "seb_alink.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

static const char *TAG = "alink";
#define HEAD_RING  (8192)                // сэмплов (~85 мс стерео 48 кГц)
#define TX_RB      (8 * 1024)            // байт Bluetooth к отправке

static seb_ring_t s_head;
static RingbufHandle_t s_txrb;
static seb_alink_rx_t s_rx;
static volatile int64_t s_duck_ms;
static volatile bool s_duck_cmd;
static bool s_up;

seb_ring_t *cpal_head_ring(void) { return &s_head; }
bool cpal_duck(void) { return s_duck_cmd || (s_duck_ms && now_ms() - s_duck_ms < 300); }
void cpal_set_duck(bool on) { s_duck_cmd = on; }

void cpal_bt_push(const uint8_t *d, uint32_t len)
{
    if (s_txrb && len) xRingbufferSend(s_txrb, d, len, 0);    // не успеваем — теряем, BT не тормозим
}

static void on_pkt(const seb_apkt_t *p, void *ctx)
{
    if (p->type != SEB_ALINK_T_PCM || p->len < 4) return;
    if (p->flags & SEB_ALINK_F_DUCK) s_duck_ms = now_ms();
    uint32_t n = p->len / 4 * 2;
    if (seb_ring_space(&s_head) >= n) seb_ring_write(&s_head, (const int16_t *)p->payload, n);
}

static void rx_task(void *arg)
{
    static uint8_t buf[1024];
    for (;;) {
        int n = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (n > 0) seb_alink_rx_feed(&s_rx, buf, n, on_pkt, NULL);
    }
}

static void tx_task(void *arg)
{
    static uint8_t pkt[SEB_ALINK_MAX_PAYLOAD + SEB_ALINK_OVERHEAD];
    static uint8_t pl[SEB_ALINK_MAX_PAYLOAD];
    uint16_t seq = 0;
    for (;;) {
        size_t have = 0;
        while (have < SEB_ALINK_MAX_PAYLOAD) {          // собрать 256 кадров (кольцо может отдать двумя кусками)
            size_t got = 0;
            void *p = xRingbufferReceiveUpTo(s_txrb, &got, pdMS_TO_TICKS(50), SEB_ALINK_MAX_PAYLOAD - have);
            if (!p) break;
            memcpy(pl + have, p, got);
            vRingbufferReturnItem(s_txrb, p);
            have += got;
        }
        have &= ~(size_t)3;                             // целые стерео-кадры
        if (!have) continue;
        size_t n = seb_alink_pack(pkt, SEB_ALINK_T_PCM, 0, (uint16_t)bta_rate(), seq++, pl, (uint16_t)have);
        uart_write_bytes(UART_NUM_1, pkt, n);
    }
}

void cpal_report(void)
{
    link_send("A,%lu,%lu,%lu", (unsigned long)s_rx.ok, (unsigned long)s_rx.crc_err, (unsigned long)s_rx.lost);
}

void cpal_init(void)
{
    int16_t *hb = heap_caps_malloc(HEAD_RING * 2, MALLOC_CAP_8BIT);
    s_txrb = xRingbufferCreate(TX_RB, RINGBUF_TYPE_BYTEBUF);
    if (!hb || !s_txrb) { ESP_LOGE(TAG, "нет памяти под аудио-связь"); return; }
    seb_ring_init(&s_head, hb, HEAD_RING);
    seb_alink_rx_init(&s_rx);
    uart_config_t c = {
        .baud_rate = SEB_ALINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM_1, 4096, 2048, 0, NULL, 0) != ESP_OK) { ESP_LOGE(TAG, "UART1 не встал"); return; }
    uart_param_config(UART_NUM_1, &c);
    uart_set_pin(UART_NUM_1, PIN_ALINK_TX, PIN_ALINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // ВАЖНО: на другом конце провода GPIO46 головы — strapping-пин, при старте/прошивке головы он должен быть LOW.
    gpio_pullup_dis(PIN_ALINK_RX);
    gpio_pulldown_en(PIN_ALINK_RX);
    uart_set_rx_full_threshold(UART_NUM_1, 96);
    s_up = true;
    xTaskCreatePinnedToCore(rx_task, "alink_rx", 3072, NULL, 14, NULL, 1);
    xTaskCreatePinnedToCore(tx_task, "alink_tx", 3072, NULL, 13, NULL, 1);
    ESP_LOGI(TAG, "аудио-связь: TX GPIO%d -> голова 1, RX GPIO%d <- голова 46, %d бод", PIN_ALINK_TX, PIN_ALINK_RX, SEB_ALINK_BAUD);
}

void cpal_sleep(void)
{
    if (s_up) uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
    gpio_reset_pin(PIN_ALINK_TX);
    gpio_set_direction(PIN_ALINK_TX, GPIO_MODE_INPUT);
    gpio_pulldown_en(PIN_ALINK_RX);
}
