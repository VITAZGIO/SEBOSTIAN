#include "audio.h"
#include <string.h>
#include <math.h>
#include "board.h"
#include "state.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "audio";

#define BLOCK        240                 // 5 мс при 48 кГц
#define DMA_DESC     6
#define REF_TAPS     47                  // ФНЧ ~7 кГц перед прореживанием 48 -> 16 кГц
#define MP3_RING     (48000 * 2)         // сэмплов (стерео): ~1 с

const float AUDIO_VOLS[] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
const int AUDIO_NVOL = sizeof(AUDIO_VOLS) / sizeof(AUDIO_VOLS[0]);

typedef struct {
    int16_t *pcm;
    uint32_t frames;
    uint8_t ch;
    uint32_t rate;
} pcm_job_t;

typedef struct {
    QueueHandle_t q;
    pcm_job_t cur;
    uint32_t pos;
    bool has, starved;
    seb_rs_t rs;
    uint32_t rs_rate;
    int rs_ch;
    volatile bool flush_req;
    volatile bool busy;
} jsrc_t;

static i2s_chan_handle_t s_tx;
static jsrc_t s_src[SRC_COUNT];

// MP3
static seb_ring_t s_mp3;
static seb_rs_t s_mp3_rs;
static volatile int s_mp3_rate = 44100, s_mp3_ch = 2;
static volatile uint32_t s_mp3_gen, s_mp3_gen_seen;
static volatile bool s_mp3_flush;

// AEC reference
static seb_ring_t s_ref;
static float s_fir[REF_TAPS];
static float s_fir_hist[REF_TAPS * 2];
static int s_fir_pos, s_dec_phase;
static volatile float s_ref_lvl;

// состояние выходов
static volatile int64_t s_last_sound_ms;
static volatile int64_t s_user_speaking_ms;
static volatile bool s_ready;
static int s_pin_spk = -1, s_pin_aux = -1;
static float s_duck = 1.0f;
static seb_lim_t s_lim;

// статистика TTS
static volatile bool s_sess;
static volatile int64_t s_first_ms, s_lastp_ms;
static volatile int s_gaps, s_pieces;

// ---------------- очереди PCM ----------------
bool audio_push(audio_src_t src, int16_t *pcm, uint32_t frames, int ch, int rate, int wait_ms)
{
    pcm_job_t j = {.pcm = pcm, .frames = frames, .ch = (uint8_t)ch, .rate = (uint32_t)rate};
    if (!pcm || !frames || (ch != 1 && ch != 2) || rate < 4000) { free(pcm); return false; }
    s_src[src].busy = true;
    if (xQueueSend(s_src[src].q, &j, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        free(pcm);
        return false;
    }
    return true;
}

void audio_flush(audio_src_t src)
{
    s_src[src].flush_req = true;
    for (int i = 0; i < 50 && s_src[src].flush_req; i++) vTaskDelay(pdMS_TO_TICKS(5));
}

bool audio_src_busy(audio_src_t src)
{
    jsrc_t *s = &s_src[src];
    return s->has || uxQueueMessagesWaiting(s->q) > 0;
}

void audio_wait_idle(audio_src_t src, int max_ms)
{
    int64_t t0 = now_ms();
    while (audio_src_busy(src) && now_ms() - t0 < max_ms) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(audio_out_latency_ms() + 20));      // хвост из DMA
}

void audio_tts_session(bool on)
{
    if (on) { s_first_ms = 0; s_lastp_ms = 0; s_gaps = 0; s_pieces = 0; }
    s_sess = on;
}
int64_t audio_tts_first_ms(void) { return s_first_ms; }
int64_t audio_tts_last_ms(void) { return s_lastp_ms; }
int audio_tts_gaps(void) { return s_gaps; }
int audio_tts_pieces(void) { return s_pieces; }

static void jsrc_drop(jsrc_t *s)
{
    if (s->has) { free(s->cur.pcm); s->has = false; }
    pcm_job_t j;
    while (xQueueReceive(s->q, &j, 0) == pdTRUE) free(j.pcm);
    s->starved = true;
}

// Набрать frames стерео-кадров (float) из очереди кусков; возвращает сколько набрал.
static int jsrc_pull(jsrc_t *s, float *out, int frames, bool tts)
{
    if (s->flush_req) { jsrc_drop(s); s->flush_req = false; }
    int made = 0;
    while (made < frames) {
        if (!s->has) {
            if (xQueueReceive(s->q, &s->cur, 0) != pdTRUE) {
                if (made == 0 && !s->starved) s->starved = true;
                break;
            }
            s->has = true;
            s->pos = 0;
            if (s->starved || s->cur.rate != s->rs_rate || s->cur.ch != s->rs_ch) {
                seb_rs_init(&s->rs, s->cur.ch, s->cur.rate, AUDIO_RATE);
                s->rs_rate = s->cur.rate;
                s->rs_ch = s->cur.ch;
            }
            if (tts && s_sess) {
                if (s_pieces > 0 && s->starved) s_gaps++;
                if (!s_first_ms) s_first_ms = now_ms();
            }
            s->starved = false;
        }
        int used = 0;
        int m = seb_rs_process(&s->rs, s->cur.pcm + s->pos * s->cur.ch, (int)(s->cur.frames - s->pos), &used,
                               out + 2 * made, frames - made);
        s->pos += used;
        made += m;
        if (s->pos >= s->cur.frames) {
            free(s->cur.pcm);
            s->has = false;
            if (tts && s_sess) { s_pieces++; s_lastp_ms = now_ms(); }
        } else if (m == 0 && used == 0) {
            break;
        }
    }
    s->busy = s->has || uxQueueMessagesWaiting(s->q) > 0;
    return made;
}

// ---------------- MP3 ----------------
seb_ring_t *audio_mp3_ring(void) { return &s_mp3; }

void audio_mp3_format(int rate, int ch)
{
    s_mp3_rate = rate;
    s_mp3_ch = ch;
    s_mp3_gen++;
}

void audio_mp3_flush(void)
{
    s_mp3_flush = true;
    for (int i = 0; i < 50 && s_mp3_flush; i++) vTaskDelay(pdMS_TO_TICKS(5));
}

static int mp3_pull(float *out, int frames)
{
    if (s_mp3_flush) { seb_ring_clear(&s_mp3); s_mp3_flush = false; s_mp3_gen_seen = s_mp3_gen - 1; }
    if (s_mp3_gen_seen != s_mp3_gen) {
        s_mp3_gen_seen = s_mp3_gen;
        seb_rs_init(&s_mp3_rs, s_mp3_ch, s_mp3_rate, AUDIO_RATE);
    }
    int ch = s_mp3_ch, made = 0;
    while (made < frames) {
        const int16_t *p;
        uint32_t n = seb_ring_peek(&s_mp3, &p);
        int inf = (int)(n / ch);
        if (inf <= 0) break;
        int used = 0;
        int m = seb_rs_process(&s_mp3_rs, p, inf, &used, out + 2 * made, frames - made);
        seb_ring_consume(&s_mp3, used * ch);
        made += m;
        if (!m && !used) break;
    }
    return made;
}

// ---------------- выходы ----------------
bool audio_active(void) { return now_ms() - s_last_sound_ms < 300; }
void audio_user_speaking(bool on) { if (on) s_user_speaking_ms = now_ms(); else s_user_speaking_ms = 0; }

bool audio_spk_on(void)
{
    // AUX-режим со вставленным штекером: голова в AUX не звучит (шины раздельные),
    // поэтому её собственный звук (голос, эффекты, MP3) всё равно идёт в динамики.
    if (g_st.bat_dead) return false;
    return !g_st.spk_muted && (g_set.out_mode != OUT_AUX || !g_st.aux_plug || audio_active());
}

bool audio_aux_on(void) { return !g_st.bat_dead && !g_st.aux_muted && g_st.aux_plug && g_set.out_mode != OUT_SPK; }

static void apply_pins(void)
{
    if (!s_ready) return;
    int spk = audio_spk_on() ? 1 : 0, aux = audio_aux_on() ? 1 : 0;
    if (spk != s_pin_spk) { gpio_set_level(PIN_SPK_MUTE, spk ? 0 : 1); s_pin_spk = spk; }
    if (aux != s_pin_aux) { gpio_set_level(PIN_XSMT, aux ? 1 : 0); s_pin_aux = aux; }
}

void audio_apply_outputs(void)
{
    apply_pins();
    ESP_LOGI(TAG, "вывод %s: динамики %s, AUX %s (штекер %s)",
             g_set.out_mode == OUT_SPK ? "DINAMIKI" : g_set.out_mode == OUT_AUX ? "AUX" : "OBA",
             audio_spk_on() ? "ON" : "OFF", audio_aux_on() ? "ON" : "OFF", g_st.aux_plug ? "есть" : "нет");
}

void audio_volume_step(int dir)
{
    int cur = 0;
    for (int i = 0; i < AUDIO_NVOL; i++) if (fabsf(AUDIO_VOLS[i] - g_set.volume) < 0.01f) cur = i;
    int n = cur + dir;
    if (n < 0) n = 0;
    if (n >= AUDIO_NVOL) n = AUDIO_NVOL - 1;
    if (AUDIO_VOLS[n] > g_set.spk_ceil + 0.001f) {          // выше потолка не даём
        st_flash(COL_YELLOW, "Потолок громкости %.2f", g_set.spk_ceil);
        return;
    }
    g_set.volume = AUDIO_VOLS[n];
    settings_save();
    ESP_LOGI(TAG, "громкость %.2f", g_set.volume);
}

// ---------------- reference для AEC ----------------
seb_ring_t *audio_ref_ring(void) { return &s_ref; }
int audio_out_latency_ms(void) { return DMA_DESC * BLOCK * 1000 / AUDIO_RATE; }
float audio_ref_level(void) { return s_ref_lvl; }

static void fir_design(void)
{
    const float fc = 7000.0f / AUDIO_RATE;      // нормированная частота среза
    const int M = REF_TAPS - 1;
    float sum = 0;
    for (int i = 0; i < REF_TAPS; i++) {
        float n = i - M / 2.0f;
        float h = n == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * n) / ((float)M_PI * n);
        h *= 0.54f - 0.46f * cosf(2 * (float)M_PI * i / M);       // окно Хэмминга
        s_fir[i] = h;
        sum += h;
    }
    for (int i = 0; i < REF_TAPS; i++) s_fir[i] /= sum;
}

// стерео float (уже с громкостью) -> моно 16 кГц в кольцо reference
static void ref_push(const float *lr, int frames, bool spk)
{
    int16_t o[BLOCK / 3 + 2];
    int no = 0;
    float e = 0;
    for (int i = 0; i < frames; i++) {
        float m = spk ? 0.5f * (lr[2 * i] + lr[2 * i + 1]) : 0.0f;
        s_fir_hist[s_fir_pos] = s_fir_hist[s_fir_pos + REF_TAPS] = m;    // двойной буфер — без % в свёртке
        s_fir_pos = (s_fir_pos + 1) % REF_TAPS;
        if (++s_dec_phase >= 3) {
            s_dec_phase = 0;
            const float *h = s_fir_hist + s_fir_pos;
            float acc = 0;
            for (int k = 0; k < REF_TAPS; k++) acc += h[k] * s_fir[REF_TAPS - 1 - k];
            int v = (int)lrintf(acc * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            o[no++] = (int16_t)v;
            e += acc * acc;
        }
    }
    if (no) s_ref_lvl = sqrtf(e / no) * 32767.0f;
    if (seb_ring_space(&s_ref) < (uint32_t)no) seb_ring_consume(&s_ref, no);   // никто не читает — выбрасываем старое
    seb_ring_write(&s_ref, o, no);
}

// ---------------- задача микшера ----------------
static void mixer_task(void *arg)
{
    static float mix[2 * BLOCK], tmp[2 * BLOCK];
    static int16_t out[2 * BLOCK];
    int64_t t_start = now_ms();
    for (;;) {
        memset(mix, 0, sizeof(mix));
        bool sound = false;
        for (int k = 0; k < SRC_COUNT; k++) {
            int n = jsrc_pull(&s_src[k], tmp, BLOCK, k == SRC_TTS);
            for (int i = 0; i < 2 * n; i++) mix[i] += tmp[i];
            if (n) sound = true;
        }
        bool voice = s_src[SRC_TTS].busy || (s_user_speaking_ms && now_ms() - s_user_speaking_ms < 1500);
        float duck_target = voice ? 0.25f : 1.0f;
        int n = mp3_pull(tmp, BLOCK);
        if (n) sound = true;
        for (int i = 0; i < n; i++) {
            s_duck += (duck_target - s_duck) * 0.002f;      // плавно, ~50 мс
            mix[2 * i] += tmp[2 * i] * s_duck;
            mix[2 * i + 1] += tmp[2 * i + 1] * s_duck;
        }
        if (sound) s_last_sound_ms = now_ms();

        float vol = g_set.volume;
        if (vol > g_set.spk_ceil) vol = g_set.spk_ceil;
        const float k = vol / 32768.0f;
        for (int i = 0; i < 2 * BLOCK; i++) mix[i] *= k;
        s_lim.thr = g_set.spk_ceil > 0.05f ? g_set.spk_ceil : 0.05f;   // сумма источников не выше потолка
        seb_lim_process(&s_lim, mix, BLOCK);
        for (int i = 0; i < 2 * BLOCK; i++) {
            int v = (int)lrintf(mix[i] * 32767.0f);
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        size_t wr = 0;
        i2s_channel_write(s_tx, out, sizeof(out), &wr, portMAX_DELAY);

        if (!s_ready && now_ms() - t_start > 250) {          // I2S уже тактирует — можно снимать мьют
            s_ready = true;
            audio_apply_outputs();
        }
        apply_pins();
        ref_push(mix, BLOCK, s_pin_spk == 1);
    }
}

void audio_init(void)
{
    for (int k = 0; k < SRC_COUNT; k++) {
        s_src[k].q = xQueueCreate(32, sizeof(pcm_job_t));
        s_src[k].starved = true;
    }
    int16_t *mp3buf = heap_caps_malloc(MP3_RING * 2, MALLOC_CAP_SPIRAM);
    seb_ring_init(&s_mp3, mp3buf, MP3_RING);
    int16_t *refbuf = heap_caps_malloc(16000 * 2, MALLOC_CAP_SPIRAM);      // 1 с
    seb_ring_init(&s_ref, refbuf, 16000);
    fir_design();
    seb_lim_init(&s_lim, 0.35f, AUDIO_RATE);
    seb_rs_init(&s_mp3_rs, 2, 44100, AUDIO_RATE);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC;
    cc.dma_frame_num = BLOCK;
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_SPK_BCLK,
            .ws = PIN_SPK_LRC,
            .dout = PIN_SPK_DIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    xTaskCreatePinnedToCore(mixer_task, "mixer", 6144, NULL, 20, NULL, 1);
    ESP_LOGI(TAG, "I2S1 -> MAX98357A: %d Гц стерео, задержка DMA %d мс", AUDIO_RATE, audio_out_latency_ms());
}
