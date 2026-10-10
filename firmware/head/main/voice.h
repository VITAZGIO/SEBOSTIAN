// Голос: разговор с сервером Себастьяна (§6 ТЗ) и все долгие действия с микрофоном/звуком.
// Одна рабочая задача (worker) выполняет задания по очереди: ASK, PTT, PING, тесты, TRAIN,
// ответ на фразу/текст после слова «Себастьян».
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    JOB_ASK,            // кнопка ASK: запись до тапа (макс 8 с) -> /ask_stream
    JOB_PTT,            // кнопка мика на со-процессоре: держи и говори
    JOB_PING,           // GET /health
    JOB_MIC_TEST,
    JOB_LR_TEST,
    JOB_KPOS, JOB_KNEG, JOB_KBG, JOB_KPLAY, JOB_KDEL,
    JOB_PHRASE,         // готовая фраза-команда (после «дзынь») -> /ask_stream
    JOB_TEXT,           // «Себастьян, включи свет» -> /ask_text_stream
} job_type_t;

typedef struct {
    job_type_t type;
    int16_t *pcm;       // JOB_PHRASE: фраза 16 кГц (worker освободит)
    int n;
    int64_t t_end;      // когда кончилась речь (для таймингов)
    float stt_s;        // сколько распознавали на нашей стороне (wake)
    char *text;         // JOB_TEXT: команда (worker освободит)
} job_t;

void voice_init(void);
bool voice_post(const job_t *j);
bool voice_busy(void);
bool voice_mic_exclusive(void);     // worker сам пишет микрофон — слушатель молчит
void voice_abort(void);             // перебили: прервать ответ и замолчать
// /stt: фраза -> текст. Возвращает HTTP-код (-1 нет связи).
int voice_stt(const int16_t *pcm, int n, char *text, size_t cap);
