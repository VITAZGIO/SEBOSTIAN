#include "rf433.h"
#include <string.h>
#include "board.h"
#include "cp.h"
#include "led.h"
#include "seb_util.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "rf";
#define PULSE_US   389
#define BITS       24
#define REPEAT     8

static const uint32_t RF_TRIG[8] = {1382424, 1382420, 1382428, 1382418, 1382426, 1382422, 1382423, 1382431};
static const uint32_t RF_LIGHT_BASE = 9348096;      // код подсветки #n = 9348096 + n

static seb_rc_rx_t s_rx;
static volatile int64_t s_last_edge;
static volatile int64_t s_mute_until;
static volatile bool s_rx_on;
static QueueHandle_t s_rxq, s_txq;
static rmt_channel_handle_t s_tx;
static rmt_encoder_handle_t s_copy;

typedef struct { int n; } tx_job_t;     // 1..21 код, 22 HOLD

static void rx_isr(void *arg)
{
    int64_t t = esp_timer_get_time();
    uint32_t d = (uint32_t)(t - s_last_edge);
    s_last_edge = t;
    if (!s_rx_on) return;
    seb_rc_code_t c;
    if (seb_rc_rx_edge(&s_rx, d, &c)) {
        BaseType_t w = pdFALSE;
        xQueueSendFromISR(s_rxq, &c, &w);
        if (w) portYIELD_FROM_ISR();
    }
}

void rf433_rx_enable(bool on) { s_rx_on = on; }

static void send_code(uint32_t code)
{
    uint16_t hi[BITS + 1], lo[BITS + 1];
    int n = seb_rc_tx_encode(code, BITS, PULSE_US, hi, lo, BITS + 1);
    rmt_symbol_word_t sym[BITS + 1];
    for (int i = 0; i < n; i++) {
        sym[i].level0 = 1;
        sym[i].duration0 = hi[i];
        sym[i].level1 = 0;
        sym[i].duration1 = lo[i];
    }
    s_rx_on = false;                           // чтобы не поймать самих себя
    rmt_transmit_config_t tc = {.loop_count = 0, .flags.eot_level = 0};
    for (int r = 0; r < REPEAT; r++) rmt_transmit(s_tx, s_copy, sym, n * sizeof(rmt_symbol_word_t), &tc);
    rmt_tx_wait_all_done(s_tx, 2000);          // 8 повторов ≈ 0.4 с
    s_mute_until = now_ms() + 300;
    seb_rc_rx_init(&s_rx);
    s_rx_on = !g_ota_mode;
}

static void tx_task(void *arg)
{
    tx_job_t j;
    for (;;) {
        if (xQueueReceive(s_txq, &j, portMAX_DELAY) != pdTRUE) continue;
        led_fill(255, 120, 0);                 // оранжевая вспышка = передаю
        if (j.n >= 1 && j.n <= 21) {
            uint32_t code = RF_LIGHT_BASE + j.n;
            send_code(code);
            ESP_LOGI(TAG, "TX #%d код %lu", j.n, (unsigned long)code);
            link_send("X,%d,%lu", j.n, (unsigned long)code);
        } else if (j.n == 22) {
            // «HOLD» = код #1 три раза подряд (так устройство его и понимает — проверено:
            // 3 нажатия за секунду срабатывают, а длинная непрерывная пачка — нет)
            for (int k = 0; k < 3; k++) {
                send_code(RF_LIGHT_BASE + 1);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            link_send("X,22,%lu", (unsigned long)(RF_LIGHT_BASE + 1));
            ESP_LOGI(TAG, "HOLD готово");
        }
        led_off();
    }
}

static void rx_task(void *arg)
{
    seb_rc_code_t c;
    int64_t trig_last[8] = {0}, light_last = 0, unk_last = 0;
    for (;;) {
        if (xQueueReceive(s_rxq, &c, portMAX_DELAY) != pdTRUE) continue;
        int64_t t = now_ms();
        if (t < s_mute_until) continue;
        // монитор видит всё, что ловит приёмник — удобно понять, работает ли он вообще
        ESP_LOGI(TAG, "RX %lu  бит:%d импульс:%dмкс", (unsigned long)c.code, c.bits, c.delay_us);
        if (c.bits != BITS) continue;          // фильтр: 433-приёмник ловит много мусора
        bool done = false;
        for (int i = 0; i < 8; i++) {
            if (c.code != RF_TRIG[i]) continue;
            done = true;
            if (t - trig_last[i] < 1000) break;          // антидребезг: пульт шлёт код пачкой
            trig_last[i] = t;
            ESP_LOGI(TAG, ">>> ТРИГГЕР %d <<<", i + 1);
            link_send("R,%d,%lu", i + 1, (unsigned long)c.code);
            led_flash(0, 255, 0, 1, 80, 0);
            break;
        }
        if (done) continue;
        if (c.code > RF_LIGHT_BASE && c.code <= RF_LIGHT_BASE + 21) {
            if (t - light_last < 300) continue;
            light_last = t;
            link_send("L,%d,%lu", (int)(c.code - RF_LIGHT_BASE), (unsigned long)c.code);
            continue;
        }
        if (t - unk_last < 500) continue;
        unk_last = t;
        link_send("U,%lu", (unsigned long)c.code);
    }
}

void rf433_send_light(int n)
{
    if (n < 1 || n > 21 || !s_txq) return;
    tx_job_t j = {n};
    xQueueSend(s_txq, &j, 0);
}

void rf433_hold(void)
{
    if (!s_txq) return;
    ESP_LOGI(TAG, "HOLD: код #1 x3");
    tx_job_t j = {22};
    xQueueSend(s_txq, &j, 0);
}

void rf433_init(void)
{
    s_rxq = xQueueCreate(8, sizeof(seb_rc_code_t));
    s_txq = xQueueCreate(8, sizeof(tx_job_t));
    seb_rc_rx_init(&s_rx);
    gpio_hold_dis(PIN_RF_TX);
    rmt_tx_channel_config_t cc = {
        .gpio_num = PIN_RF_TX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,              // 1 тик = 1 мкс
        .mem_block_symbols = 64,
        .trans_queue_depth = REPEAT,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cc, &s_tx));
    rmt_copy_encoder_config_t ec = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&ec, &s_copy));
    ESP_ERROR_CHECK(rmt_enable(s_tx));

    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_RF_RX, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_ANYEDGE};
    gpio_config(&in);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_RF_RX, rx_isr, NULL);
    s_rx_on = true;
    xTaskCreatePinnedToCore(rx_task, "rf_rx", 3072, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(tx_task, "rf_tx", 3072, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "радио 433: RX=%d TX=%d, протокол 1, %d мкс, %d бита", PIN_RF_RX, PIN_RF_TX, PULSE_US, BITS);
}
