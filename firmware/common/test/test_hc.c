// Тест HTTP-клиента (seb_hc) + разбора SSE против макета сервера Себастьяна (mock_server.py).
// Тот же порядок разбора, что в head/main/voice.c: строки SSE -> JSON -> base64 -> WAV.
#include "unity.h"
#include "seb_hc.h"
#include "seb_util.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define HOST "127.0.0.1"
static int PORT = 18099;

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    int stt, text, audio, done, chunks;
    size_t audio_frames;
    int rate;
    char you[256], seb[256];
} sse_res_t;

static void parse_stream(seb_hc_t *h, sse_res_t *r)
{
    static char lbufmem[2 * 1024 * 1024];
    seb_linebuf_t lb;
    seb_linebuf_init(&lb, lbufmem, sizeof(lbufmem));
    char rb[4096];
    memset(r, 0, sizeof(*r));
    time_t t0 = time(NULL);
    while (!r->done && time(NULL) - t0 < 20) {
        int n = seb_hc_read(h, rb, sizeof(rb));
        if (n == -2) continue;
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (!seb_linebuf_put(&lb, rb[i])) continue;
            if (!lb.overflow && !strncmp(lb.buf, "data:", 5)) {
                const char *js = lb.buf + 5;
                char type[16] = "";
                seb_json_str(js, "type", type, sizeof(type));
                if (!strcmp(type, "stt")) { r->stt++; seb_json_str(js, "text", r->you, sizeof(r->you)); }
                else if (!strcmp(type, "text")) {
                    r->text++;
                    seb_json_str(js, "text", r->seb, sizeof(r->seb));
                    float c;
                    if (seb_json_num(js, "chunks", &c)) r->chunks = (int)c;
                } else if (!strcmp(type, "audio")) {
                    const char *q = seb_json_val(js, "audio_b64");
                    TEST_ASSERT_NOT_NULL(q);
                    char *b = (char *)q + 1;
                    char *e = strchr(b, '"');
                    TEST_ASSERT_NOT_NULL(e);
                    size_t bytes = seb_b64_decode_inplace(b, (size_t)(e - b));
                    seb_wav_info_t wi;
                    TEST_ASSERT_TRUE(seb_wav_parse((uint8_t *)b, bytes, 16000, &wi));
                    r->rate = wi.rate;
                    r->audio_frames += wi.data_len / 2;
                    r->audio++;
                } else if (!strcmp(type, "done")) r->done++;
            }
            seb_linebuf_reset(&lb);
        }
    }
}

static void ask(const char *mode)
{
    seb_hc_t h = {0};
    char path[64];
    snprintf(path, sizeof(path), "/ask_text_stream?mode=%s", mode);
    const char *body = "{\"text\":\"как дела\"}";
    TEST_ASSERT_TRUE(seb_hc_request(&h, HOST, PORT, "POST", path, "application/json", "text/event-stream", (int)strlen(body), 3000));
    TEST_ASSERT_TRUE(seb_hc_send(&h, body, (int)strlen(body)));
    TEST_ASSERT_EQUAL(200, seb_hc_head(&h, 10000));
    sse_res_t r;
    parse_stream(&h, &r);
    seb_hc_close(&h);
    TEST_ASSERT_EQUAL(1, r.stt);
    TEST_ASSERT_EQUAL_STRING("Себастьян, как дела?", r.you);
    TEST_ASSERT_EQUAL(1, r.text);
    TEST_ASSERT_EQUAL_STRING("Всё отлично, сэр.", r.seb);
    TEST_ASSERT_EQUAL(2, r.chunks);
    TEST_ASSERT_EQUAL(2, r.audio);
    TEST_ASSERT_EQUAL(24000, r.rate);
    TEST_ASSERT_EQUAL(24000 * 22 / 10, r.audio_frames);      // 1.5 с + 0.7 с
    TEST_ASSERT_EQUAL(1, r.done);
}

static void test_sse_chunked(void) { ask("chunked"); }
static void test_sse_close(void) { ask("close"); }
static void test_sse_length(void) { ask("length"); }

static void test_health(void)
{
    seb_hc_t h = {0};
    TEST_ASSERT_TRUE(seb_hc_request(&h, HOST, PORT, "GET", "/health", NULL, NULL, -1, 3000));
    TEST_ASSERT_EQUAL(200, seb_hc_head(&h, 3000));
    char body[256];
    seb_hc_read_body(&h, body, sizeof(body), 3000);
    seb_hc_close(&h);
    char model[32];
    TEST_ASSERT_TRUE(seb_json_str(body, "model", model, sizeof(model)));
    TEST_ASSERT_EQUAL_STRING("qwen2.5:7b", model);
}

static void test_stt_multipart(void)
{
    // тот же multipart, что voice.c post_wav()
    static const char HEAD[] = "--B\r\nContent-Disposition: form-data; name=\"file\"; filename=\"q.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    static const char TAIL[] = "\r\n--B--\r\n";
    uint8_t wav[44 + 3200] = {0};
    seb_wav_header(wav, 16000, 3200);
    int clen = (int)(strlen(HEAD) + sizeof(wav) + strlen(TAIL));
    seb_hc_t h = {0};
    TEST_ASSERT_TRUE(seb_hc_request(&h, HOST, PORT, "POST", "/stt", "multipart/form-data; boundary=B", "application/json", clen, 3000));
    TEST_ASSERT_TRUE(seb_hc_send(&h, HEAD, (int)strlen(HEAD)));
    TEST_ASSERT_TRUE(seb_hc_send(&h, wav, sizeof(wav)));
    TEST_ASSERT_TRUE(seb_hc_send(&h, TAIL, (int)strlen(TAIL)));
    TEST_ASSERT_EQUAL(200, seb_hc_head(&h, 5000));
    char body[256], text[128], cmd[128];
    seb_hc_read_body(&h, body, sizeof(body), 3000);
    seb_hc_close(&h);
    TEST_ASSERT_TRUE(seb_json_str(body, "text", text, sizeof(text)));
    TEST_ASSERT_TRUE(seb_wake_split(text, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("включи свет", cmd);
}

static volatile bool g_abort;
static void *set_abort(void *a) { (void)a; usleep(700000); g_abort = true; return NULL; }

static void test_abort(void)
{
    seb_hc_t h = {0};
    g_abort = false;
    h.abort = &g_abort;
    TEST_ASSERT_TRUE(seb_hc_request(&h, HOST, PORT, "GET", "/slow", NULL, NULL, -1, 3000));
    pthread_t th;
    pthread_create(&th, NULL, set_abort, NULL);
    time_t t0 = time(NULL);
    TEST_ASSERT_EQUAL(-1, seb_hc_head(&h, 20000));           // прервали — не ждём 20 с
    TEST_ASSERT_LESS_OR_EQUAL(2, (int)(time(NULL) - t0));
    pthread_join(th, NULL);
    seb_hc_close(&h);
}

static void test_connect_fail(void)
{
    seb_hc_t h = {0};
    TEST_ASSERT_FALSE(seb_hc_request(&h, HOST, 1, "GET", "/", NULL, NULL, -1, 1000));
}

int main(int argc, char **argv)
{
    if (argc > 1) PORT = atoi(argv[1]);
    UNITY_BEGIN();
    RUN_TEST(test_health);
    RUN_TEST(test_stt_multipart);
    RUN_TEST(test_sse_chunked);
    RUN_TEST(test_sse_close);
    RUN_TEST(test_sse_length);
    RUN_TEST(test_abort);
    RUN_TEST(test_connect_fail);
    return UNITY_END();
}
