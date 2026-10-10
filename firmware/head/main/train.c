#include "train.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <math.h>
#include "state.h"
#include "sdcard.h"
#include "mic.h"
#include "audio.h"
#include "fx.h"
#include "display.h"
#include "seb_util.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kws";
#define CLIP_MS  2000
#define BG_S     30
#define SERIES   10

static int s_pos_max, s_neg_max, s_bg_max;

// похожие слова и обычная речь — чтобы модель НЕ срабатывала на них
static const char *NEG_WORDS[] = {
    "Севастополь", "себестоимость", "бастион", "Кристиан", "Степан", "Станислав",
    "Святослав", "бассейн", "себе", "себя", "сегодня", "Сева, стой",
    "Татьяна", "всё ясно", "включи свет", "как дела", "Сергей", "себе стану",
    "бастард", "Сабина", "есть что поесть", "любое слово"};
static const int NEG_N = sizeof(NEG_WORDS) / sizeof(NEG_WORDS[0]);

const char *KWS_TIPS[] = {         // одна строка экрана — до ~33 букв
    "POS: 100+ раз: обычно/тихо/громко",
    "POS: близко, 1 м, 2-3 м, по кругу",
    "POS: быстро, медленно, интонации",
    "POS: при шуме: ТВ, музыка, речь",
    "NEG: 60+ раз — слово с экрана",
    "ФОН: 5-10 раз по 30 с (ТВ, музыка)",
    "Друг скажет «Себастьян» — тоже POS",
};
const int KWS_TIPS_N = sizeof(KWS_TIPS) / sizeof(KWS_TIPS[0]);

static void kset(uint32_t col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void kset(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    st_lock();
    vsnprintf(g_st.kws_state, sizeof(g_st.kws_state), fmt, ap);
    g_st.kws_state_col = col;
    st_unlock();
    va_end(ap);
}

static void prompt(const char *big, const char *small, uint32_t col)
{
    st_lock();
    snprintf(g_st.kws_big, sizeof(g_st.kws_big), "%s", big);
    snprintf(g_st.kws_small, sizeof(g_st.kws_small), "%s", small);
    g_st.kws_prompt_col = col;
    st_unlock();
}

static int count_dir(const char *dir, int *max_idx)
{
    *max_idx = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int c = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_type == DT_DIR || l < 8 || strcasecmp(e->d_name + l - 4, ".wav")) continue;
        c++;
        int k = atoi(e->d_name + l - 8);
        if (k > *max_idx) *max_idx = k;
    }
    closedir(d);
    return c;
}

void train_scan(void)
{
    if (!sdcard_ok()) return;
    sd_mkdir(SD_MOUNT "/kws");
    sd_mkdir(SD_MOUNT "/kws/pos");
    sd_mkdir(SD_MOUNT "/kws/neg");
    sd_mkdir(SD_MOUNT "/kws/bg");
    g_st.kws_pos = count_dir(SD_MOUNT "/kws/pos", &s_pos_max);
    g_st.kws_neg = count_dir(SD_MOUNT "/kws/neg", &s_neg_max);
    g_st.kws_bg = count_dir(SD_MOUNT "/kws/bg", &s_bg_max);
}

static bool write_wav(const char *path, const int16_t *pcm, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t h[44];
    seb_wav_header(h, 16000, (uint32_t)n * 2);
    bool ok = fwrite(h, 1, 44, f) == 44 && fwrite(pcm, 2, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static void record(int16_t *buf, int n)
{
    mic_rec_begin(true);                       // сырой звук — таким его потом услышит модель
    int got = mic_rec_read(buf, n, n / 16 + 2000);
    mic_rec_end();
    if (got < n) memset(buf + got, 0, (size_t)(n - got) * 2);
}

static void level(const int16_t *b, int n, int *pk, int *rms)
{
    double sq = 0;
    *pk = 0;
    for (int i = 0; i < n; i++) { int v = abs(b[i]); if (v > *pk) *pk = v; sq += (double)b[i] * b[i]; }
    *rms = (int)sqrt(sq / n);
}

static bool tap_abort(int ms)
{
    int64_t t = now_ms();
    while (now_ms() - t < ms) {
        if (touch_down_now()) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void set_last(const char *path)
{
    st_lock();
    snprintf(g_st.kws_last, sizeof(g_st.kws_last), "%s", path);
    st_unlock();
}

void train_series(int kind)
{
    if (!sdcard_ok()) { kset(COL_RED, "НЕТ SD-карты!"); return; }
    if (!g_st.mic_ok) return;
    const int N = 16000 * CLIP_MS / 1000;
    int16_t *buf = heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
    if (!buf) { kset(COL_RED, "нет PSRAM"); return; }
    train_scan();
    int64_t hi = now_ms(), t0 = now_ms();         // дождаться, пока отпустишь кнопку
    while (now_ms() - hi < 200 && now_ms() - t0 < 3000) { if (touch_down_now()) hi = now_ms(); vTaskDelay(pdMS_TO_TICKS(5)); }
    int done = 0;
    for (int s = 0; s < SERIES; s++) {
        const char *word = kind == 0 ? "СЕБАСТЬЯН" : NEG_WORDS[(g_st.kws_neg + s) % NEG_N];
        char small[64];
        snprintf(small, sizeof(small), "после «пик» — %d/%d  (тап = стоп)", s + 1, SERIES);
        prompt(word, small, kind == 0 ? COL_GREEN : COL_YELLOW);
        kset(COL_CYAN, kind == 0 ? "POS: скажи после «пик»" : "NEG: скажи слово с экрана");
        if (tap_abort(500)) break;
        fx_pik(kind == 0 ? 1000 : 700, 90);
        kset(COL_RED, "ЗАПИСЬ 2 с...");
        record(buf, N);
        int pk, rms;
        level(buf, N, &pk, &rms);
        char name[64];
        if (kind == 0) snprintf(name, sizeof(name), SD_MOUNT "/kws/pos/p_%04d.wav", ++s_pos_max);
        else snprintf(name, sizeof(name), SD_MOUNT "/kws/neg/n_%04d.wav", ++s_neg_max);
        if (!write_wav(name, buf, N)) { kset(COL_RED, "ошибка записи на SD"); break; }
        set_last(name);
        if (kind == 0) g_st.kws_pos++;
        else g_st.kws_neg++;
        done++;
        ST_SET(kws_info, "%s  пик %d%s", name + strlen(SD_MOUNT) + 5, pk, pk < 300 ? "  ТИХО!" : pk > 32000 ? "  ПЕРЕГРУЗ!" : "  ok");
        ESP_LOGI(TAG, "%s пик %d rms %d", name, pk, rms);
        if (tap_abort(300)) break;
    }
    free(buf);
    prompt("", "", COL_WHITE);
    kset(COL_GREEN, "записано %d. Ещё? POS / NEG / ФОН", done);
}

void train_background(void)
{
    if (!sdcard_ok()) { kset(COL_RED, "НЕТ SD-карты!"); return; }
    if (!g_st.mic_ok) return;
    const int N = 16000 * BG_S;
    int16_t *buf = heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
    if (!buf) { kset(COL_RED, "нет PSRAM"); return; }
    train_scan();
    prompt("ФОН 30 с", "НЕ говори «Себастьян». ТВ/музыка/разговор — можно", COL_ORANGE);
    kset(COL_RED, "ЗАПИСЬ ФОНА 30 с...");
    vTaskDelay(pdMS_TO_TICKS(300));
    record(buf, N);
    int pk, rms;
    level(buf, N, &pk, &rms);
    char name[64];
    snprintf(name, sizeof(name), SD_MOUNT "/kws/bg/b_%04d.wav", ++s_bg_max);
    bool ok = write_wav(name, buf, N);
    free(buf);
    if (ok) {
        g_st.kws_bg++;
        set_last(name);
        ST_SET(kws_info, "%s  пик %d  rms %d", name + strlen(SD_MOUNT) + 5, pk, rms);
    }
    prompt("", "", COL_WHITE);
    if (ok) kset(COL_GREEN, "фон записан");
    else kset(COL_RED, "ошибка записи на SD");
}

void train_play_last(void)
{
    char path[64];
    st_copy(path, sizeof(path), g_st.kws_last);
    if (!path[0]) { kset(COL_YELLOW, "нечего играть"); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { kset(COL_RED, "файл не открылся"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 44 || sz > 2 * 1024 * 1024) { fclose(f); return; }
    int n = (int)(sz - 44) / 2;
    int16_t *p = heap_caps_malloc((size_t)n * 2, MALLOC_CAP_SPIRAM);
    if (!p) { fclose(f); return; }
    fseek(f, 44, SEEK_SET);
    fread(p, 2, n, f);
    fclose(f);
    int pk = 1;
    for (int i = 0; i < n; i++) if (abs(p[i]) > pk) pk = abs(p[i]);
    float g = 20000.0f / pk;
    if (g > 40.0f) g = 40.0f;
    for (int i = 0; i < n; i++) {
        int v = (int)(p[i] * g);
        p[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    kset(COL_CYAN, "играет %s", path + strlen(SD_MOUNT) + 5);
    audio_push(SRC_FX, p, n, 1, 16000, 300);
    audio_wait_idle(SRC_FX, 35000);
    kset(COL_WHITE, "готово");
}

void train_del_last(void)
{
    char path[64];
    st_copy(path, sizeof(path), g_st.kws_last);
    if (!path[0]) { kset(COL_YELLOW, "нечего удалять"); return; }
    if (remove(path) == 0) {
        kset(COL_YELLOW, "удалено %s", path + strlen(SD_MOUNT) + 5);
        set_last("");
        ST_SET(kws_info, " ");
    } else {
        kset(COL_RED, "не удалось удалить");
    }
    train_scan();
}

void train_next_tip(void) { g_st.kws_tip = (g_st.kws_tip + 1) % KWS_TIPS_N; }
