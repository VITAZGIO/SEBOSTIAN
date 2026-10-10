#include "mic.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "board.h"
#include "state.h"
#include "settings.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_aec.h"
#include "esp_ns.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx;
static seb_ring_t s_listen, s_rec;
static volatile bool s_rec_on, s_rec_raw;
static aec_handle_t *s_aec;
static ns_handle_t s_ns;
static int s_chunk = 256;
static volatile bool s_aec_active;

seb_ring_t *mic_listen_ring(void) { return &s_listen; }
bool mic_aec_active(void) { return s_aec_active; }

void mic_rec_begin(bool raw)
{
    s_rec_on = false;
    s_rec_raw = raw;
    seb_ring_clear(&s_rec);
    s_rec_on = true;
}

int mic_rec_read(int16_t *dst, int max, int timeout_ms)
{
    int64_t t0 = now_ms();
    int got = 0;
    while (got < max) {
        got += (int)seb_ring_read(&s_rec, dst + got, (uint32_t)(max - got));
        if (got >= max || now_ms() - t0 >= timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return got;
}

void mic_rec_end(void) { s_rec_on = false; }

static inline int16_t mic16(int32_t raw)
{
    int32_t v = raw >> 14;           // как в Arduino: INMP441 24 бит в 32-битном слоте
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void push(seb_ring_t *r, const int16_t *x, int n)
{
    if (seb_ring_space(r) < (uint32_t)n) return;     // читатель не успевает — кадр выбрасываем
    seb_ring_write(r, x, n);
}

// Отладка AEC: задержка эха относительно reference по взаимной корреляции (раз в ~3 с)
#define XC_N    2048
#define XC_LAG  1600     // до 100 мс
static int16_t *s_xc_mic, *s_xc_ref;
static int s_xc_pos;
static volatile bool s_xc_ready;

static void xcorr_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_xc_ready) continue;
        // ref[i] сопоставлен mic[i] уже с поправкой; ищем, где эхо: mic[i] ~ ref[i - lag]
        float best = 0, e_ref = 0, e_mic = 0;
        int best_lag = 0;
        for (int i = XC_LAG; i < XC_N; i++) { e_ref += (float)s_xc_ref[i] * s_xc_ref[i]; e_mic += (float)s_xc_mic[i] * s_xc_mic[i]; }
        if (e_ref > 1e6f && e_mic > 1e5f) {
            for (int lag = 0; lag < XC_LAG; lag += 2) {
                float acc = 0;
                for (int i = XC_LAG; i < XC_N; i += 2) acc += (float)s_xc_mic[i] * s_xc_ref[i - lag];
                if (fabsf(acc) > best) { best = fabsf(acc); best_lag = lag; }
            }
            g_st.aec_delay_ms = best_lag / 16.0f;
        }
        s_xc_ready = false;
    }
}

static void mic_task(void *arg)
{
    const int N = s_chunk;
    int32_t *raw = heap_caps_malloc(N * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *m = heap_caps_aligned_calloc(16, N, 2, MALLOC_CAP_INTERNAL);
    int16_t *ref = heap_caps_aligned_calloc(16, N, 2, MALLOC_CAP_INTERNAL);
    int16_t *out = heap_caps_aligned_calloc(16, N, 2, MALLOC_CAP_INTERNAL);
    int16_t *ns_acc = heap_caps_malloc((N + 160) * 2, MALLOC_CAP_INTERNAL);
    int ns_n = 0;
    seb_ring_t *rr = audio_ref_ring();
    double e_in = 0, e_out = 0;
    int e_cnt = 0;
    for (;;) {
        size_t br = 0;
        if (i2s_channel_read(s_rx, raw, N * 4, &br, pdMS_TO_TICKS(200)) != ESP_OK || br < (size_t)N * 4) continue;
        int32_t pk = 0;
        for (int i = 0; i < N; i++) {
            m[i] = mic16(raw[i]);
            int a = abs(m[i]);
            if (a > pk) pk = a;
        }
        float lv = g_st.mic_level * 0.85f;
        g_st.mic_level = pk > lv ? pk : lv;

        // reference: держим в кольце столько, чтобы забранный кусок совпадал по времени с эхом
        int lead = g_set.aec_lead_ms;
        int target = N + (audio_out_latency_ms() - lead) * 16;
        if (target < N) target = N;
        uint32_t fill = seb_ring_fill(rr);
        if (fill > (uint32_t)target) seb_ring_consume(rr, fill - target);
        fill = seb_ring_fill(rr);
        if (fill >= (uint32_t)N) seb_ring_read(rr, ref, N);
        else { memset(ref, 0, N * 2); seb_ring_read(rr, ref + (N - fill), fill); }

        const int16_t *clean = m;
        bool use_aec = s_aec && g_set.aec_on;
        s_aec_active = use_aec;
        if (use_aec) {
            aec_process(s_aec, m, ref, out);
            clean = out;
            double a = 0, b = 0, r = 0;
            for (int i = 0; i < N; i++) { a += (double)m[i] * m[i]; b += (double)out[i] * out[i]; r += (double)ref[i] * ref[i]; }
            if (r / N > 1e4) { e_in += a; e_out += b; e_cnt++; }       // считаем только когда динамики звучат
            if (e_cnt >= 60) {
                g_st.aec_erle_db = (float)(10.0 * log10((e_in + 1) / (e_out + 1)));
                e_in = e_out = 0;
                e_cnt = 0;
            }
            g_st.aec_ref_lvl = audio_ref_level();
            if (!s_xc_ready && s_xc_mic) {                              // копим окно для оценки задержки
                for (int i = 0; i < N && s_xc_pos < XC_N; i++, s_xc_pos++) { s_xc_mic[s_xc_pos] = m[i]; s_xc_ref[s_xc_pos] = ref[i]; }
                if (s_xc_pos >= XC_N) { s_xc_pos = 0; s_xc_ready = true; }
            }
        }

        if (s_ns && g_set.ns_on) {                                      // шумодав кусками по 10 мс
            memcpy(ns_acc + ns_n, clean, N * 2);
            ns_n += N;
            int done = 0;
            while (ns_n - done >= 160) {
                ns_process(s_ns, ns_acc + done, ns_acc + done);
                push(&s_listen, ns_acc + done, 160);
                if (s_rec_on && !s_rec_raw) push(&s_rec, ns_acc + done, 160);
                done += 160;
            }
            memmove(ns_acc, ns_acc + done, (ns_n - done) * 2);
            ns_n -= done;
        } else {
            push(&s_listen, clean, N);
            if (s_rec_on && !s_rec_raw) push(&s_rec, clean, N);
        }
        if (s_rec_on && s_rec_raw) push(&s_rec, m, N);
    }
}

bool mic_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 256;
    if (i2s_new_channel(&cc, NULL, &s_rx) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_SD,
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;          // L/R микрофона на GND
    if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) return false;
    if (i2s_channel_enable(s_rx) != ESP_OK) return false;

    s_aec = aec_create(MIC_RATE, 4, 1, AEC_MODE_SR_HIGH_PERF);
    if (s_aec) {
        s_chunk = aec_get_chunksize(s_aec);
        ESP_LOGI(TAG, "эхоподавление ESP-SR готово: кадр %d сэмплов (%d мс)", s_chunk, s_chunk / 16);
        g_st.aec_ok = true;
    } else {
        ESP_LOGE(TAG, "AEC не создался — слушаю без эхоподавления");
    }
    s_ns = ns_pro_create(10, 1, MIC_RATE);
    int16_t *lb = heap_caps_malloc(16000 * 2 * 2, MALLOC_CAP_SPIRAM);      // 2 с
    int16_t *rb = heap_caps_malloc(16000 * 2 * 2, MALLOC_CAP_SPIRAM);
    if (!lb || !rb) return false;
    seb_ring_init(&s_listen, lb, 16000 * 2);
    seb_ring_init(&s_rec, rb, 16000 * 2);
    s_xc_mic = heap_caps_malloc(XC_N * 2, MALLOC_CAP_SPIRAM);
    s_xc_ref = heap_caps_malloc(XC_N * 2, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(mic_task, "mic", 6144, NULL, 19, NULL, 1);
    xTaskCreatePinnedToCore(xcorr_task, "aec_dbg", 3072, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "INMP441 на I2S0: SCK %d, WS %d, SD %d, %d Гц", PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD, MIC_RATE);
    return true;
}
