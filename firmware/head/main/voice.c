#include "voice.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "state.h"
#include "settings.h"
#include "audio.h"
#include "mic.h"
#include "mp3.h"
#include "fx.h"
#include "net.h"
#include "ui.h"
#include "train.h"
#include "display.h"
#include "seb_util.h"
#include "seb_hc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "voice";
#define REC_MAX_S      8
#define LCAP           (2 * 1024 * 1024)     // строка SSE: кусок wav в base64 бывает 300+ КБ
#define FIRST_WAIT_MS  45000                 // первый ответ после простоя ~25 с (холодная Ollama)
#define TOTAL_MS       120000
#define BOUNDARY       "----sebastianS3"

static QueueHandle_t s_q;
static volatile bool s_busy, s_excl, s_abort;
static char *s_lbuf;

bool voice_busy(void) { return s_busy; }
bool voice_mic_exclusive(void) { return s_excl; }

void voice_abort(void)
{
    if (!s_busy) return;
    ESP_LOGW(TAG, "перебили — прерываю ответ");
    s_abort = true;
    audio_flush(SRC_TTS);
}

bool voice_post(const job_t *j)
{
    if (s_busy && (j->type == JOB_ASK || j->type == JOB_PTT)) return false;
    return xQueueSend(s_q, j, 0) == pdTRUE;
}

static void vset(uint32_t col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void vset(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    st_lock();
    vsnprintf(g_st.v_state, sizeof(g_st.v_state), fmt, ap);
    g_st.v_state_col = col;
    st_unlock();
    va_end(ap);
}

// ---------------- WAV ----------------
// wav+44 уже содержит n сэмплов 16 кГц: ФВЧ ~120 Гц + усиление по RMS + мягкий лимитер (для Whisper)
static size_t build_wav(uint8_t *wav, int n)
{
    int16_t *rec = (int16_t *)(wav + 44);
    float hp = 0, prev = 0;
    double sq = 0;
    for (int i = 0; i < n; i++) {
        float x = rec[i];
        hp = 0.954f * (hp + x - prev);
        prev = x;
        rec[i] = (int16_t)fmaxf(-32768, fminf(32767, hp));
        sq += (double)hp * hp;
    }
    float g = 3000.0f / (sqrtf((float)(sq / (n ? n : 1))) + 1);
    g = fminf(fmaxf(g, 1.0f), 30.0f);
    for (int i = 0; i < n; i++) rec[i] = (int16_t)(tanhf(rec[i] * g / 32767.0f) * 30000.0f);
    seb_wav_header(wav, 16000, (uint32_t)n * 2);
    return 44 + (size_t)n * 2;
}

// ---------------- HTTP ----------------
// свой мини-клиент на сокетах (common/seb_hc.c): Content-Length, chunked и «до закрытия»,
// чтение кусками по 0.3 с — видно прерывание (s_abort)
typedef seb_hc_t hconn_t;

static bool hc_request(hconn_t *h, const char *method, const char *path, const char *ctype, const char *accept, int clen)
{
    h->abort = &s_abort;
    return seb_hc_request(h, g_set.srv_host, g_set.srv_port, method, path, ctype, accept, clen, 3000);
}
#define hc_send seb_hc_send
#define hc_head seb_hc_head
#define hc_read seb_hc_read
#define hc_close seb_hc_close
#define read_body seb_hc_read_body

static const char MP_HEAD[] = "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"q.wav\"\r\n"
                              "Content-Type: audio/wav\r\n\r\n";
static const char MP_TAIL[] = "\r\n--" BOUNDARY "--\r\n";

// POST multipart (поле "file"): тело = head + wav + tail
static bool post_wav(hconn_t *h, const char *path, const uint8_t *wav, size_t len, const char *accept)
{
    int clen = (int)(strlen(MP_HEAD) + len + strlen(MP_TAIL));
    if (!hc_request(h, "POST", path, "multipart/form-data; boundary=" BOUNDARY, accept, clen)) return false;
    if (!hc_send(h, MP_HEAD, strlen(MP_HEAD)) || !hc_send(h, wav, (int)len) || !hc_send(h, MP_TAIL, strlen(MP_TAIL))) {
        hc_close(h);
        return false;
    }
    return true;
}

// ---------------- /health ----------------
static void do_ping(void)
{
    if (!net_connected()) { ST_SET(v_srv, "Сервер: ОШИБКА — нет Wi-Fi"); return; }
    ST_SET(v_srv, "Сервер: пинг...");
    int64_t t0 = now_ms();
    static hconn_t h;
    if (!hc_request(&h, "GET", "/health", NULL, NULL, -1)) { ST_SET(v_srv, "Сервер: ОШИБКА — нет связи с %s", g_set.srv_host); return; }
    int code = hc_head(&h, 3000);
    char body[512];
    read_body(&h, body, sizeof(body), 3000);
    hc_close(&h);
    int dt = (int)(now_ms() - t0);
    char model[64] = "";
    seb_json_str(body, "model", model, sizeof(model));
    if (code == 200) ST_SET(v_srv, "Сервер: OK %d мс  %s", dt, model);
    else ST_SET(v_srv, "Сервер: ОШИБКА HTTP %d", code);
    ESP_LOGI(TAG, "/health -> %d за %d мс", code, dt);
}

// ---------------- /stt ----------------
int voice_stt(const int16_t *pcm, int n, char *text, size_t cap)
{
    text[0] = 0;
    uint8_t *wav = heap_caps_malloc(44 + (size_t)n * 2, MALLOC_CAP_SPIRAM);
    if (!wav) return -1;
    memcpy(wav + 44, pcm, (size_t)n * 2);
    size_t wl = build_wav(wav, n);
    hconn_t *h = heap_caps_malloc(sizeof(hconn_t), MALLOC_CAP_SPIRAM);
    bool ok = h && post_wav(h, "/stt", wav, wl, "application/json");
    free(wav);
    if (!ok) { free(h); return -1; }
    int code = hc_head(h, 20000);
    char *body = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (body) read_body(h, body, 8192, 5000);
    hc_close(h);
    free(h);
    if (!body) return -1;
    char *b = body;
    while (*b == ' ' || *b == '\n' || *b == '\r') b++;
    if (code == 200 && *b == '{' && seb_json_str(b, "text", text, cap)) {       // {"text": "..."}
    } else if (*b == '"') {                                                     // иногда просто строка в кавычках
        size_t l = strlen(b);
        while (l && (b[l - 1] == '\n' || b[l - 1] == '\r' || b[l - 1] == ' ')) l--;
        if (l >= 2 && b[l - 1] == '"') l--;
        size_t k = l - 1 < cap - 1 ? l - 1 : cap - 1;
        memcpy(text, b + 1, k);
        text[k] = 0;
    } else {
        strncpy(text, b, cap - 1);
        text[cap - 1] = 0;
    }
    free(body);
    return code;
}

// ---------------- ответ (SSE) ----------------
// кусок wav (после base64) -> PCM -> в очередь микшера
static bool queue_wav(const uint8_t *w, size_t n)
{
    seb_wav_info_t wi;
    if (!seb_wav_parse(w, n, 24000, &wi)) { ESP_LOGW(TAG, "wav не понял: bits=%d ch=%d", wi.bits, wi.channels); return false; }
    size_t frames = wi.data_len / (2 * wi.channels);
    if (!frames) return false;
    int16_t *pcm = heap_caps_malloc(frames * 2 * wi.channels, MALLOC_CAP_SPIRAM);
    if (!pcm) { ESP_LOGE(TAG, "нет PSRAM под кусок"); return false; }
    memcpy(pcm, wi.data, frames * 2 * wi.channels);
    return audio_push(SRC_TTS, pcm, (uint32_t)frames, wi.channels, wi.rate, 5000);
}

// tRef = конец речи; rec_s — длина записи; t_up — отправка; esp_stt — распознавание на нашей стороне (wake)
static void stream_answer(hconn_t *c, int64_t t_ref, float rec_s, int64_t t_up, float esp_stt)
{
    float s_stt = -1, s_brain = -1, s_audio1 = -1, s_total = -1;
    int chunks = 0, pieces = 0;
    bool ok = false;
    char err[96] = "";
    vset(COL_YELLOW, "ДУМАЕТ... (1-й раз после простоя ~25 с)");
    audio_tts_session(true);
    int64_t t0 = now_ms();
    int code = hc_head(c, FIRST_WAIT_MS);       // ждём заголовки (сервер может долго думать)
    if (s_abort) snprintf(err, sizeof(err), "перебили");
    else if (code < 0) snprintf(err, sizeof(err), "сервер молчит (таймаут)");
    else if (code != 200) snprintf(err, sizeof(err), "HTTP %d", code);
    else {
        static char rb[4096];
        seb_linebuf_t lb;
        seb_linebuf_init(&lb, s_lbuf, LCAP);
        int64_t t_last = now_ms();
        bool done = false;
        while (!done && !s_abort) {
            int r = hc_read(c, rb, sizeof(rb));
            if (r == -2) {
                int64_t idle = now_ms() - t_last;
                if (idle > FIRST_WAIT_MS || now_ms() - t0 > TOTAL_MS) { snprintf(err, sizeof(err), "сервер замолчал (таймаут)"); break; }
                continue;
            }
            if (r <= 0) break;
            t_last = now_ms();
            for (int i = 0; i < r && !done; i++) {
                if (!seb_linebuf_put(&lb, rb[i])) continue;
                char *ln = lb.buf;
                if (lb.overflow) { snprintf(err, sizeof(err), "строка SSE не влезла"); seb_linebuf_reset(&lb); continue; }
                if (strncmp(ln, "data:", 5) != 0) { seb_linebuf_reset(&lb); continue; }
                const char *js = ln + 5;
                char type[16] = "";
                // тип ищем в начале строки: в audio-строке дальше идут сотни КБ base64
                char head[200];
                strncpy(head, js, sizeof(head) - 1);
                head[sizeof(head) - 1] = 0;
                if (!seb_json_str(head, "type", type, sizeof(type))) seb_json_str(js, "type", type, sizeof(type));
                if (!strcmp(type, "stt")) {
                    st_lock();
                    seb_json_str(js, "text", g_st.v_you, sizeof(g_st.v_you));
                    st_unlock();
                    seb_json_num(js, "t", &s_stt);
                    vset(COL_YELLOW, "ДУМАЕТ...");
                } else if (!strcmp(type, "text")) {
                    st_lock();
                    seb_json_str(js, "text", g_st.v_seb, sizeof(g_st.v_seb));
                    st_unlock();
                    seb_json_num(js, "t", &s_brain);
                    float cf;
                    if (seb_json_num(js, "chunks", &cf)) chunks = (int)cf;
                    vset(COL_GREEN, "ГОВОРИТ...");
                } else if (!strcmp(type, "audio")) {
                    if (s_audio1 < 0) seb_json_num(js, "t_audio", &s_audio1);
                    const char *q = seb_json_val(js, "audio_b64");
                    if (q && *q == '"') {
                        char *b = (char *)q + 1;
                        char *e = strchr(b, '"');
                        if (e) {
                            if (!g_st.v_seb[0]) {      // текст ответа мог прийти только в audio-строке
                                *e = 0;
                                st_lock();
                                seb_json_str(e + 1, "text", g_st.v_seb, sizeof(g_st.v_seb));
                                st_unlock();
                            }
                            size_t bytes = seb_b64_decode_inplace(b, (size_t)(e - b));
                            if (queue_wav((uint8_t *)b, bytes)) pieces++;
                        }
                    }
                    if (chunks) vset(COL_GREEN, "ГОВОРИТ... кусок %d/%d", pieces, chunks);
                    else vset(COL_GREEN, "ГОВОРИТ... кусок %d", pieces);
                } else if (!strcmp(type, "error")) {
                    char m[80] = "";
                    seb_json_str(js, "message", m, sizeof(m));
                    snprintf(err, sizeof(err), "%s", m);
                } else if (!strcmp(type, "done")) {
                    seb_json_num(js, "total", &s_total);
                    ok = true;
                    done = true;
                }
                seb_linebuf_reset(&lb);
            }
        }
        if (s_abort) snprintf(err, sizeof(err), "перебили");
        else if (!ok && !err[0]) snprintf(err, sizeof(err), "обрыв связи");
    }
    hc_close(c);

    // дождаться, пока доиграет всё (или перебьют)
    int64_t tw = now_ms();
    while (!s_abort && audio_src_busy(SRC_TTS) && now_ms() - tw < 120000) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_abort) audio_flush(SRC_TTS);
    else vTaskDelay(pdMS_TO_TICKS(audio_out_latency_ms() + 50));
    audio_tts_session(false);

    int64_t f = audio_tts_first_ms(), l = audio_tts_last_ms();
    float first = f ? (f - t_ref) / 1000.0f : -1, all = l ? (l - t_ref) / 1000.0f : -1;
    ST_SET(v_t1, "ЗАП %.1f | 1-й ЗВУК %.1f | ВСЁ %.1f", rec_s, first, all);
    if (esp_stt >= 0) ST_SET(v_t2, "stt %.1f мозг %.1f звук %.1f", esp_stt, s_brain, s_audio1);
    else ST_SET(v_t2, "stt %.1f мозг %.1f звук %.1f отпр %.1f", s_stt, s_brain, s_audio1, t_up / 1000.0f);
    if (err[0]) vset(COL_RED, "ОШИБКА: %s", err);
    else vset(audio_tts_gaps() ? COL_YELLOW : COL_GREEN, "ГОТОВО: кусков %d, пауз %d", audio_tts_pieces(), audio_tts_gaps());
    ESP_LOGI(TAG, "%s | %s | %s", g_st.v_state, g_st.v_t1, g_st.v_t2);
}

static bool voice_prep(void)
{
    if (!net_connected()) { vset(COL_RED, "ОШИБКА: нет Wi-Fi"); return false; }
    if (!s_lbuf) s_lbuf = heap_caps_malloc(LCAP, MALLOC_CAP_SPIRAM);
    if (!s_lbuf) { vset(COL_RED, "ОШИБКА: нет PSRAM"); return false; }
    return true;
}

static hconn_t *s_hc;

static void ask_audio(uint8_t *wav, size_t wl, int64_t t_ref, float rec_s)
{
    vset(COL_YELLOW, "ОТПРАВКА на сервер...");
    if (!post_wav(s_hc, "/ask_stream", wav, wl, "text/event-stream")) { vset(COL_RED, "ОШИБКА: нет связи с %s", g_set.srv_host); return; }
    stream_answer(s_hc, t_ref, rec_s, now_ms() - t_ref, -1);
}

static void ask_text(const char *text, int64_t t_ref, float rec_s, float esp_stt)
{
    size_t cap = strlen(text) * 2 + 32;
    char *body = malloc(cap), *esc = malloc(cap);
    if (!body || !esc) { free(body); free(esc); return; }
    seb_json_escape(text, esc, cap);
    snprintf(body, cap, "{\"text\":\"%s\"}", esc);
    free(esc);
    int bl = strlen(body);
    bool ok = hc_request(s_hc, "POST", "/ask_text_stream", "application/json", "text/event-stream", bl);
    if (ok && !hc_send(s_hc, body, bl)) { hc_close(s_hc); ok = false; }
    free(body);
    if (!ok) { vset(COL_RED, "ОШИБКА: нет связи с %s", g_set.srv_host); return; }
    stream_answer(s_hc, t_ref, rec_s, 0, esp_stt);
}

static void clear_dialog(void)
{
    st_lock();
    g_st.v_you[0] = g_st.v_seb[0] = g_st.v_t1[0] = g_st.v_t2[0] = 0;
    st_unlock();
}

// ---------------- ASK / PTT: вопрос голосом ----------------
static void do_ask(bool ptt)
{
    if (!g_st.mic_ok || !voice_prep()) return;
    const int NMAX = 16000 * REC_MAX_S;
    uint8_t *wav = heap_caps_malloc(44 + NMAX * 2, MALLOC_CAP_SPIRAM);
    if (!wav) { vset(COL_RED, "ОШИБКА: нет PSRAM"); return; }
    bool was = mp3_playing();
    mp3_stop();
    ui_goto(PG_VOICE);
    clear_dialog();
    if (!ptt) {                             // дождаться, пока палец отпустит кнопку ASK
        int64_t hi = now_ms(), t0 = now_ms();
        while (now_ms() - hi < 150 && now_ms() - t0 < 3000) {
            if (touch_down_now()) hi = now_ms();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    s_excl = true;
    int16_t *rec = (int16_t *)(wav + 44);
    vset(COL_RED, ptt ? "ЗАПИСЬ... (отпусти кнопку = стоп)" : "ЗАПИСЬ... говори (тап = стоп)");
    mic_rec_begin(false);
    int64_t t_rec0 = now_ms(), t_show = 0;
    int n = 0;
    while (n < NMAX && !s_abort) {
        n += mic_rec_read(rec + n, (NMAX - n) < 320 ? (NMAX - n) : 320, 100);
        int64_t el = now_ms() - t_rec0;
        if (now_ms() - t_show > 300) {
            t_show = now_ms();
            vset(COL_RED, "ЗАПИСЬ %.1f с  (макс %d)", el / 1000.0f, REC_MAX_S);
        }
        if (ptt) { if ((el > 300 && !g_st.cp_mic) || !link_rx_ok()) break; }
        else if (el > 500 && touch_down_now()) break;
    }
    mic_rec_end();
    s_excl = false;
    int64_t t_end = now_ms();
    if (n < 8000) {
        vset(COL_YELLOW, "слишком коротко (<0.5 с)");
    } else {
        size_t wl = build_wav(wav, n);
        ask_audio(wav, wl, t_end, n / 16000.0f);
    }
    free(wav);
    if (was) mp3_play();
}

// ---------------- задача ----------------
static void worker_task(void *arg)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;
        s_abort = false;
        g_st.voice_busy = true;
        switch (j.type) {
        case JOB_ASK: do_ask(false); break;
        case JOB_PTT: do_ask(true); break;
        case JOB_PING: do_ping(); break;
        case JOB_MIC_TEST:
        case JOB_LR_TEST: {
            bool was = mp3_playing();
            mp3_stop();
            s_excl = true;
            if (j.type == JOB_MIC_TEST) fx_mic_test();
            else fx_lr_test();
            s_excl = false;
            if (was) mp3_play();
        } break;
        case JOB_KPOS: case JOB_KNEG: case JOB_KBG: case JOB_KPLAY:
            s_excl = true;
            mp3_stop();
            if (j.type == JOB_KPOS) train_series(0);
            else if (j.type == JOB_KNEG) train_series(1);
            else if (j.type == JOB_KBG) train_background();
            else train_play_last();
            s_excl = false;
            break;
        case JOB_KDEL: train_del_last(); break;
        case JOB_PHRASE:
            if (voice_prep()) {
                clear_dialog();
                uint8_t *wav = heap_caps_malloc(44 + (size_t)j.n * 2, MALLOC_CAP_SPIRAM);
                if (wav) {
                    memcpy(wav + 44, j.pcm, (size_t)j.n * 2);
                    size_t wl = build_wav(wav, j.n);
                    ask_audio(wav, wl, j.t_end, j.n / 16000.0f);
                    free(wav);
                }
            }
            break;
        case JOB_TEXT:
            if (voice_prep()) {
                st_lock();
                g_st.v_seb[0] = g_st.v_t1[0] = g_st.v_t2[0] = 0;
                strncpy(g_st.v_you, j.text, sizeof(g_st.v_you) - 1);
                g_st.v_you[sizeof(g_st.v_you) - 1] = 0;
                st_unlock();
                ask_text(j.text, j.t_end, j.n / 16000.0f, j.stt_s);
            }
            break;
        }
        free(j.pcm);
        free(j.text);
        s_abort = false;
        s_busy = false;
        g_st.voice_busy = false;
    }
}

void voice_init(void)
{
    s_q = xQueueCreate(4, sizeof(job_t));
    s_hc = heap_caps_malloc(sizeof(hconn_t), MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(worker_task, "voice", 8192, NULL, 8, NULL, 0);
}
