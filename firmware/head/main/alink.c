#include "alink.h"
#include <string.h>
#include "board.h"
#include "state.h"
#include "seb_alink.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "alink";
#define BT_RING   (44100 * 2)            // сэмплов: ~1 с стерео
#define AUX_RING  (48000)                // сэмплов: ~0.5 с стерео

static seb_ring_t s_bt, s_aux;
static volatile int s_bt_rate;
static volatile int64_t s_bt_last;
static volatile bool s_duck;
static seb_alink_rx_t s_rx;

seb_ring_t *alink_bt_ring(void) { return &s_bt; }
int alink_bt_rate(void) { return s_bt_rate; }
int64_t alink_bt_last_ms(void) { return s_bt_last; }
void alink_set_duck(bool on) { s_duck = on; }

void alink_aux_push(const int16_t *lr, int frames)
{
    if (!s_aux.buf) return;
    uint32_t n = (uint32_t)frames * 2;
    if (seb_ring_space(&s_aux) < n) return;      // связь не успевает — лучше пропуск, чем растущая задержка
    seb_ring_write(&s_aux, lr, n);
}

static void on_pkt(const seb_apkt_t *p, void *ctx)
{
    if (p->type != SEB_ALINK_T_PCM || p->len < 4) return;
    if (p->rate != s_bt_rate) {                  // новая частота — старое выбрасываем
        seb_ring_clear(&s_bt);
        s_bt_rate = p->rate;
        g_st.al_bt_rate = p->rate;
    }
    uint32_t n = p->len / 4 * 2;
    if (seb_ring_space(&s_bt) >= n) seb_ring_write(&s_bt, (const int16_t *)p->payload, n);
    s_bt_last = now_ms();
}

static void rx_task(void *arg)
{
    static uint8_t buf[2048];
    for (;;) {
        int n = uart_read_bytes(ALINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (n > 0) {
            seb_alink_rx_feed(&s_rx, buf, n, on_pkt, NULL);
            g_st.al_rx_ok = s_rx.ok;
            g_st.al_rx_crc = s_rx.crc_err;
            g_st.al_rx_lost = s_rx.lost;
        }
    }
}

static void tx_task(void *arg)
{
    static int16_t pcm[SEB_ALINK_MAX_PAYLOAD / 2];
    static uint8_t pkt[SEB_ALINK_MAX_PAYLOAD + SEB_ALINK_OVERHEAD];
    uint16_t seq = 0;
    for (;;) {
        if (seb_ring_fill(&s_aux) < SEB_ALINK_MAX_PAYLOAD / 2) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        seb_ring_read(&s_aux, pcm, SEB_ALINK_MAX_PAYLOAD / 2);
        size_t n = seb_alink_pack(pkt, SEB_ALINK_T_PCM, s_duck ? SEB_ALINK_F_DUCK : 0, 48000, seq++, pcm, SEB_ALINK_MAX_PAYLOAD);
        uart_write_bytes(ALINK_UART, pkt, n);
        g_st.al_tx_pkts++;
    }
}

void alink_init(void)
{
    int16_t *bb = heap_caps_malloc(BT_RING * 2, MALLOC_CAP_SPIRAM);
    int16_t *ab = heap_caps_malloc(AUX_RING * 2, MALLOC_CAP_SPIRAM);
    if (!bb || !ab) { ESP_LOGE(TAG, "нет PSRAM"); return; }
    seb_ring_init(&s_bt, bb, BT_RING);
    seb_ring_init(&s_aux, ab, AUX_RING);
    seb_alink_rx_init(&s_rx);
    uart_config_t c = {
        .baud_rate = SEB_ALINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(ALINK_UART, 16384, 8192, 0, NULL, 0) != ESP_OK) { ESP_LOGE(TAG, "UART2 не встал"); return; }
    uart_param_config(ALINK_UART, &c);
    uart_set_pin(ALINK_UART, PIN_ALINK_TX, PIN_ALINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_pulldown_en(PIN_ALINK_RX);              // провода нет — линия не болтается
    uart_set_rx_full_threshold(ALINK_UART, 96);
    xTaskCreatePinnedToCore(rx_task, "alink_rx", 4096, NULL, 18, NULL, 0);
    xTaskCreatePinnedToCore(tx_task, "alink_tx", 4096, NULL, 17, NULL, 0);
    ESP_LOGI(TAG, "аудио-связь: RX GPIO%d <- сопроц. 23, TX GPIO%d -> сопроц. 19, %d бод", PIN_ALINK_RX, PIN_ALINK_TX,
             SEB_ALINK_BAUD);
}
