#include "listener.h"
#include <string.h>
#include <stdlib.h>
#include "state.h"
#include "settings.h"
#include "mic.h"
#include "audio.h"
#include "mp3.h"
#include "net.h"
#include "fx.h"
#include "ui.h"
#include "voice.h"
#include "sysmon.h"
#include "seb_util.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wake";
#define FRAME       320                     // 20 мс
#define PRE_N       (16000 * 400 / 1000)    // предзапись 400 мс
#define SEG_N       (16000 * 8)             // фраза до 8 с
#define CMD_WAIT_MS 8000                    // после «дзынь» ждём команду

typedef struct {
    int16_t *pcm;
    int n;
    int64_t t_end;
    int mode;
} phrase_t;

enum { LM_WAKE = 0, LM_CMD = 1 };

static QueueHandle_t s_pq;
static volatile bool s_started, s_hold = true;
static volatile int64_t s_cmd_deadline;
static seb_vad_t s_vad;

bool listener_started(void) { return s_started; }

void listener_start(void)
{
    if (s_started) return;
    s_started = true;
    g_st.ls_running = true;
    ESP_LOGI(TAG, "слушатель «Себастьян» запущен");
}

static void listen_task(void *arg)
{
    int16_t *pre = heap_caps_calloc(PRE_N, 2, MALLOC_CAP_SPIRAM);
    int16_t *seg = heap_caps_malloc(SEG_N * 2, MALLOC_CAP_SPIRAM);
    int16_t f[FRAME];
    int fill = 0, pre_pos = 0, n = 0;
    seb_vad_cfg_t cfg;
    seb_vad_default_cfg(&cfg);
    seb_vad_init(&s_vad, &cfg);
    seb_ring_t *r = mic_listen_ring();
    bool was_hold = true;
    for (;;) {
        if (s_hold) {
            if (!was_hold) audio_user_speaking(false);
            was_hold = true;
            g_st.ls_paused = true;
            seb_ring_clear(r);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (was_hold) {                        // после паузы — начать с чистого листа (без эха ответа)
            was_hold = false;
            g_st.ls_paused = false;
            seb_ring_clear(r);
            s_vad.state = 0;
            s_vad.above = 0;
            fill = 0;
            g_st.ls_state = 0;
        }
        fill += (int)seb_ring_read(r, f + fill, FRAME - fill);
        if (fill < FRAME) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        fill = 0;
        if (s_vad.state == 0) {
            for (int i = 0; i < FRAME; i++) { pre[pre_pos] = f[i]; pre_pos = (pre_pos + 1) % PRE_N; }
        } else if (n < SEG_N) {
            int k = SEG_N - n < FRAME ? SEG_N - n : FRAME;
            memcpy(seg + n, f, k * 2);
            n += k;
        }
        // пока Себастьян говорит — порог выше (остаток эха после AEC не должен будить)
        s_vad.cfg.start_k = audio_src_busy(SRC_TTS) ? 6.0f : 3.0f;
        seb_vad_ev_t ev = seb_vad_frame(&s_vad, f, FRAME, n >= SEG_N);
        g_st.ls_nf = s_vad.nf;
        g_st.ls_lvl = s_vad.lvl;
        g_st.ls_thr = seb_vad_threshold(&s_vad);
        switch (ev) {
        case SEB_VAD_START:
            n = 0;
            for (int i = 0; i < PRE_N; i++) seg[n++] = pre[(pre_pos + i) % PRE_N];
            g_st.ls_state = 1;
            break;
        case SEB_VAD_DONE: {
            phrase_t p = {.n = n, .t_end = now_ms() - s_vad.tail_ms, .mode = g_st.ls_mode};
            p.pcm = heap_caps_malloc((size_t)n * 2, MALLOC_CAP_SPIRAM);
            if (p.pcm) {
                memcpy(p.pcm, seg, (size_t)n * 2);
                if (xQueueSend(s_pq, &p, 0) != pdTRUE) free(p.pcm);
                else g_st.ls_state = 2;
            }
            if (g_st.ls_state != 2) g_st.ls_state = 0;
            ESP_LOGI(TAG, "фраза %.1f с", n / 16000.0f);
        } break;
        case SEB_VAD_SHORT:
        case SEB_VAD_OVERFLOW:
            g_st.ls_state = 0;
            break;
        default:
            break;
        }
        audio_user_speaking(s_vad.state == 1);
    }
}

static void wake_task(void *arg)
{
    phrase_t p;
    char *text = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char *cmd = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    for (;;) {
        if (xQueueReceive(s_pq, &p, portMAX_DELAY) != pdTRUE) continue;
        float rec_s = p.n / 16000.0f;
        if (p.mode == LM_CMD) {                 // после «дзынь»: это сама команда
            g_st.ls_mode = LM_WAKE;
            if (voice_busy()) voice_abort();
            job_t j = {.type = JOB_PHRASE, .pcm = p.pcm, .n = p.n, .t_end = p.t_end};
            if (!voice_post(&j)) free(p.pcm);
            if (g_st.ls_state == 2) g_st.ls_state = 0;
            continue;
        }
        int64_t t0 = now_ms();
        int code = voice_stt(p.pcm, p.n, text, 1024);
        float stt_s = (now_ms() - t0) / 1000.0f;
        free(p.pcm);
        if (g_st.ls_state == 2) g_st.ls_state = 0;
        if (code != 200) {
            ST_SET(v_heard, "STT: HTTP %d %.40s", code, text);
            continue;
        }
        ST_SET(v_heard, "слышал: %s", text);
        ESP_LOGI(TAG, "%.1f с речи, stt %.1f с: %s", rec_s, stt_s, text);
        if (!seb_wake_split(text, cmd, 1024)) continue;
        // это к нам
        if (voice_busy()) {                     // перебили Себастьяна (или тест) — замолчать
            voice_abort();
            for (int i = 0; i < 100 && voice_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
        }
        ui_goto(PG_VOICE);
        if (cmd[0]) {                           // «Себастьян, включи свет» — сразу в мозг текстом
            job_t j = {.type = JOB_TEXT, .text = strdup(cmd), .n = p.n, .t_end = p.t_end, .stt_s = stt_s};
            if (!j.text || !voice_post(&j)) free(j.text);
        } else if (g_st.ls_state == 1 || uxQueueMessagesWaiting(s_pq) > 0) {
            // «Себастьян» … и уже говоришь дальше — следующая фраза и есть команда
            g_st.ls_mode = LM_CMD;
            s_cmd_deadline = now_ms() + CMD_WAIT_MS;
            ST_SET(v_state, "СЛУШАЮ команду...");
            g_st.v_state_col = COL_CYAN;
            // фраза уже в очереди была записана в режиме WAKE — переделать её в команду
            phrase_t q;
            if (xQueuePeek(s_pq, &q, 0) == pdTRUE) {
                xQueueReceive(s_pq, &q, 0);
                q.mode = LM_CMD;
                xQueueSendToFront(s_pq, &q, 0);
            }
        } else {                                // просто «Себастьян» и тишина — «дзынь» и ждём команду
            fx_dzyn();
            audio_wait_idle(SRC_FX, 2000);
            g_st.ls_mode = LM_CMD;
            s_cmd_deadline = now_ms() + CMD_WAIT_MS;
            ST_SET(v_state, "ДА, СЭР? говори команду");
            g_st.v_state_col = COL_CYAN;
        }
    }
}

void listener_tick(void)
{
    if (!s_started && g_set.wake_on && g_st.mic_ok && now_ms() > 20000 && !sysmon_bad_reset()) listener_start();
    bool aec = mic_aec_active();
    bool hold = !s_started || !g_set.wake_on || voice_mic_exclusive() || g_st.head_ota_pct >= 0 || !net_connected() ||
                g_st.bat_dead || g_st.grace_until;
    if (!aec) {
        // без эхоподавления: пока играет свой звук — не слушаем (как в Arduino)
        hold = hold || audio_active() || audio_src_busy(SRC_TTS) || audio_src_busy(SRC_FX) || mp3_playing() || voice_busy();
    }
    // Всё, что звучит из AUX (внешний усилитель), идёт мимо reference — его AEC не вычтет: не слушаем.
    // (Bluetooth в динамиках — можно: он в reference.)
    audio_route_t rt;
    audio_routes(&rt);
    bool aux_live = g_st.aux_plug && audio_aux_on();
    hold = hold || (aux_live && rt.bt_aux && g_st.cp_bt == 2) || (aux_live && g_st.cp_music) ||
           (aux_live && rt.head_aux && audio_head_active());
    s_hold = hold;
    if (g_st.ls_mode == LM_CMD && g_st.ls_state == 0 && !hold && now_ms() > s_cmd_deadline) {
        g_st.ls_mode = LM_WAKE;
        ST_SET(v_state, "команду не услышал");
        g_st.v_state_col = COL_YELLOW;
    }
}

void listener_init(void)
{
    s_pq = xQueueCreate(2, sizeof(phrase_t));
    xTaskCreatePinnedToCore(listen_task, "listen", 4096, NULL, 9, NULL, 1);
    xTaskCreatePinnedToCore(wake_task, "wake", 6144, NULL, 7, NULL, 0);
}
