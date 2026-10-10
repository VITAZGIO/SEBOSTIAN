#include "fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "mic.h"
#include "state.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fx";
#define FXR 48000

static int16_t *alloc_pcm(size_t samples) { return heap_caps_malloc(samples * 2 + 4, MALLOC_CAP_SPIRAM); }

// тон с плавным входом/выходом 10 мс; hz = 0 — тишина
void fx_tone(float hz, int ms, int amp, bool left, bool right)
{
    int n = FXR * ms / 1000;
    int16_t *p = alloc_pcm(n * 2);
    if (!p) return;
    const int fade = FXR / 100;
    float ph = 0, inc = 2.0f * (float)M_PI * hz / FXR;
    for (int i = 0; i < n; i++) {
        float env = i < fade ? (float)i / fade : (i > n - fade ? (float)(n - i) / fade : 1.0f);
        int16_t v = hz > 0 ? (int16_t)(amp * env * sinf(ph)) : 0;
        ph += inc;
        if (ph > 2 * (float)M_PI) ph -= 2 * (float)M_PI;
        p[2 * i] = left ? v : 0;
        p[2 * i + 1] = right ? v : 0;
    }
    audio_push(SRC_FX, p, n, 2, FXR, 1000);
}

void fx_dzyn(void)
{
    const int R = 24000, n1 = R * 9 / 100, gap = R * 4 / 100, n2 = R * 12 / 100, N = n1 + gap + n2;
    int16_t *p = alloc_pcm(N);
    if (!p) return;
    for (int i = 0; i < N; i++) {
        float f = i < n1 ? 660 : (i < n1 + gap ? 0 : 990);
        int k = i < n1 ? i : i - n1 - gap, len = i < n1 ? n1 : n2;
        float env = (k < 200) ? k / 200.0f : (k > len - 200 ? (len - k) / 200.0f : 1.0f);
        p[i] = f > 0 ? (int16_t)(20000 * env * sinf(2 * (float)M_PI * f * k / R)) : 0;
    }
    audio_push(SRC_FX, p, N, 1, R, 500);
}

void fx_pik(int hz, int ms)
{
    fx_tone(hz, ms, 18000, true, true);
    audio_wait_idle(SRC_FX, 1000);
    vTaskDelay(pdMS_TO_TICKS(150));        // хвост, чтобы «пик» не попал в запись
}

static void knocks(int n, bool l, bool r)
{
    for (int k = 0; k < n; k++) {
        fx_tone(700, 130, 12000, l, r);    // «тук»
        fx_tone(0, 220, 0, false, false);  // пауза
    }
}

void fx_lr_test(void)
{
    ESP_LOGI(TAG, "L/R: сначала ОБА (1 стук), потом ЛЕВЫЙ 3 стука, ПРАВЫЙ 2 стука");
    st_flash(COL_DKGREEN, "ОБА: 1 стук");
    knocks(1, true, true);
    fx_tone(0, 700, 0, false, false);
    audio_wait_idle(SRC_FX, 5000);
    st_flash(COL_DKGREEN, "ЛЕВЫЙ: 3 стука");
    knocks(3, true, false);
    fx_tone(0, 900, 0, false, false);
    audio_wait_idle(SRC_FX, 5000);
    st_flash(COL_DKGREEN, "ПРАВЫЙ: 2 стука");
    knocks(2, false, true);
    audio_wait_idle(SRC_FX, 5000);
}

void fx_mic_test(void)
{
    const int N = MIC_RATE * 2;
    int16_t *rec = alloc_pcm(N);
    if (!rec) return;
    st_flash(COL_RED, "ЗАПИСЬ 2 с... говори!");
    ESP_LOGI(TAG, "тест микрофона: запись 2 с");
    mic_rec_begin(true);
    int n = mic_rec_read(rec, N, 3000);
    mic_rec_end();
    int peak = 0;
    for (int i = 0; i < n; i++) if (abs(rec[i]) > peak) peak = abs(rec[i]);
    // ФВЧ ~120 Гц + усиление по RMS + мягкий лимитер (как в Arduino)
    float hp = 0, prev = 0;
    double sq = 0;
    for (int i = 0; i < n; i++) {
        float x = rec[i];
        hp = 0.954f * (hp + x - prev);
        prev = x;
        rec[i] = (int16_t)fmaxf(-32768, fminf(32767, hp));
        sq += (double)hp * hp;
    }
    float rms = sqrtf((float)(sq / (n ? n : 1))) + 1;
    float g = fminf(fmaxf(7000.0f / rms, 1.0f), 60.0f);
    for (int i = 0; i < n; i++) rec[i] = (int16_t)(tanhf(rec[i] * g / 32767.0f) * 30000.0f);
    ESP_LOGI(TAG, "записано %d, пик %d, усиление x%.1f (пик около 0 = микрофон не слышит)", n, peak, g);
    st_flash(COL_DKGREEN, "ИГРАЮ... (пик %d)", peak);
    audio_push(SRC_FX, rec, n, 1, MIC_RATE, 1000);
    audio_wait_idle(SRC_FX, 5000);
}
