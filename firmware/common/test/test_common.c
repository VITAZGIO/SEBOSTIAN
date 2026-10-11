// Юнит-тесты общих модулей на ПК (без железа). Запуск: см. firmware/common/test/README.md
#include "unity.h"
#include "seb_proto.h"
#include "seb_util.h"
#include "seb_alink.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

// ---------- протокол ----------
static void test_status_old_and_new(void)
{
    seb_cp_msg_t m;
    TEST_ASSERT_TRUE(seb_proto_parse_cp("S,0,1,0,1,3912,125,2,0", &m));   // Arduino v7.1 (8 полей)
    TEST_ASSERT_EQUAL(CP_STATUS, m.type);
    TEST_ASSERT_EQUAL(1, m.t2);
    TEST_ASSERT_EQUAL(1, m.mic);
    TEST_ASSERT_EQUAL(3912, m.bat_mv);
    TEST_ASSERT_EQUAL_UINT32(125, m.uptime);
    TEST_ASSERT_EQUAL(2, m.bt);
    TEST_ASSERT_EQUAL(-1, m.mux);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("S,1,0,0,0,0,7,0,1,1,99,100", &m));  // лишние поля — терпим
    TEST_ASSERT_EQUAL(1, m.music);
    TEST_ASSERT_EQUAL(1, m.mux);
    TEST_ASSERT_FALSE(seb_proto_parse_cp("S,1,2", &m));                     // мало полей
}

static void test_events(void)
{
    seb_cp_msg_t m;
    TEST_ASSERT_TRUE(seb_proto_parse_cp("T,3", &m));
    TEST_ASSERT_EQUAL(CP_TOUCH, m.type);
    TEST_ASSERT_EQUAL(3, m.n);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("R,2,1382420", &m));
    TEST_ASSERT_EQUAL(CP_RF_TRIG, m.type);
    TEST_ASSERT_EQUAL(2, m.n);
    TEST_ASSERT_EQUAL_UINT32(1382420, m.code);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("X,22,9348097", &m));
    TEST_ASSERT_EQUAL(CP_RF_SENT, m.type);
    TEST_ASSERT_EQUAL(22, m.n);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("U,12345", &m));
    TEST_ASSERT_EQUAL_UINT32(12345, m.code);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("W,2,0,192.168.1.182", &m));
    TEST_ASSERT_EQUAL(CP_OTA, m.type);
    TEST_ASSERT_EQUAL(2, m.ota_state);
    TEST_ASSERT_EQUAL_STRING("192.168.1.182", m.text);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("B,2,2987", &m));
    TEST_ASSERT_EQUAL(2, m.bat_level);
    TEST_ASSERT_EQUAL(2987, m.bat_mv);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("G,3412", &m));
    TEST_ASSERT_EQUAL(CP_GRACE, m.type);
    TEST_ASSERT_EQUAL(3412, m.bat_mv);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("F", &m));
    TEST_ASSERT_EQUAL(CP_FORCE_BTN, m.type);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("HELLO coproc", &m));
    TEST_ASSERT_EQUAL(CP_HELLO, m.type);
    TEST_ASSERT_EQUAL_STRING("coproc", m.text);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("P,17", &m));
    TEST_ASSERT_EQUAL(17, m.n);
    TEST_ASSERT_TRUE(seb_proto_parse_cp("V,c-idf-0.1", &m));
    TEST_ASSERT_EQUAL_STRING("c-idf-0.1", m.text);
    TEST_ASSERT_FALSE(seb_proto_parse_cp("Z,1", &m));
}

static void test_head_cmds(void)
{
    seb_hc_msg_t h;
    TEST_ASSERT_TRUE(seb_proto_parse_hc("PING 42", &h));
    TEST_ASSERT_EQUAL(HC_PING, h.type);
    TEST_ASSERT_EQUAL_UINT32(42, h.n);
    TEST_ASSERT_TRUE(seb_proto_parse_hc("RF 21", &h));
    TEST_ASSERT_EQUAL(HC_RF, h.type);
    TEST_ASSERT_EQUAL_UINT32(21, h.n);
    TEST_ASSERT_TRUE(seb_proto_parse_hc("RFHOLD", &h));
    TEST_ASSERT_EQUAL(HC_RFHOLD, h.type);
    TEST_ASSERT_TRUE(seb_proto_parse_hc("S3B 1", &h));
    TEST_ASSERT_EQUAL(HC_S3B, h.type);
    char line[300];
    TEST_ASSERT_GREATER_THAN(0, seb_proto_fmt_cfg(line, sizeof(line), "My WiFi, 5G", "p@ss word", ""));
    TEST_ASSERT_TRUE(seb_proto_parse_hc(line, &h));
    TEST_ASSERT_EQUAL(HC_CFG, h.type);
    TEST_ASSERT_EQUAL_STRING("My WiFi, 5G", h.ssid);
    TEST_ASSERT_EQUAL_STRING("p@ss word", h.pass);
    TEST_ASSERT_EQUAL_STRING("", h.ota);
    TEST_ASSERT_EQUAL(-1, seb_proto_fmt_cfg(line, 10, "abc", "def", "ghi"));
}

static void test_route_and_alink_lines(void)
{
    seb_hc_msg_t h;
    TEST_ASSERT_TRUE(seb_proto_parse_hc("AROUTE 1,0,1", &h));
    TEST_ASSERT_EQUAL(HC_AROUTE, h.type);
    TEST_ASSERT_EQUAL(1, h.bt_spk);
    TEST_ASSERT_EQUAL(0, h.bt_aux);
    TEST_ASSERT_EQUAL(1, h.head_aux);
    TEST_ASSERT_TRUE(seb_proto_parse_hc("DUCK 1", &h));
    TEST_ASSERT_EQUAL(HC_DUCK, h.type);
    TEST_ASSERT_EQUAL_UINT32(1, h.n);
    seb_cp_msg_t m;
    TEST_ASSERT_TRUE(seb_proto_parse_cp("A,1200,3,7", &m));
    TEST_ASSERT_EQUAL(CP_ALINK, m.type);
    TEST_ASSERT_EQUAL_UINT32(1200, m.a_ok);
    TEST_ASSERT_EQUAL_UINT32(3, m.a_crc);
    TEST_ASSERT_EQUAL_UINT32(7, m.a_lost);
}

// ---------- аудио-связь ----------
static int g_pk;
static uint32_t g_sum;
static void on_pkt(const seb_apkt_t *p, void *ctx)
{
    (void)ctx;
    g_pk++;
    TEST_ASSERT_EQUAL(SEB_ALINK_T_PCM, p->type);
    TEST_ASSERT_EQUAL(44100, p->rate);
    for (int i = 0; i < p->len; i++) g_sum += p->payload[i];
}

static void test_alink(void)
{
    static uint8_t stream[20 * (SEB_ALINK_MAX_PAYLOAD + SEB_ALINK_OVERHEAD) + 100];
    uint8_t pl[SEB_ALINK_MAX_PAYLOAD];
    size_t n = 0;
    uint32_t want = 0;
    stream[n++] = 0x5A;                                  // мусор перед первым пакетом
    stream[n++] = 0xA5;
    stream[n++] = 0x00;
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < (int)sizeof(pl); i++) pl[i] = (uint8_t)(i * 7 + k);
        if (k == 5) continue;                            // пакет 5 «потерян»
        size_t at = n;
        n += seb_alink_pack(stream + n, SEB_ALINK_T_PCM, 0, 44100, (uint16_t)k, pl, sizeof(pl));
        if (k == 9) stream[at + 300] ^= 0x40;            // пакет 9 испорчен — CRC не сойдётся
        else for (int i = 0; i < (int)sizeof(pl); i++) want += pl[i];
    }
    seb_alink_rx_t rx;
    seb_alink_rx_init(&rx);
    g_pk = 0;
    g_sum = 0;
    for (size_t off = 0; off < n;) {                    // кормим кусками случайной длины
        size_t k = 1 + (size_t)(rand() % 700);
        if (off + k > n) k = n - off;
        seb_alink_rx_feed(&rx, stream + off, k, on_pkt, NULL);
        off += k;
    }
    TEST_ASSERT_EQUAL(18, g_pk);
    TEST_ASSERT_EQUAL_UINT32(want, g_sum);
    TEST_ASSERT_EQUAL_UINT32(1, rx.crc_err);
    TEST_ASSERT_EQUAL_UINT32(2, rx.lost);               // 5 потерян, 9 испорчен
}

// ---------- текст ----------
static void test_b64(void)
{
    char s[] = "U2ViYXN0aWFu\nPQ==";      // "Sebastian" + "="
    size_t n = seb_b64_decode_inplace(s, strlen(s));
    TEST_ASSERT_EQUAL(10, n);
    TEST_ASSERT_EQUAL_MEMORY("Sebastian=", s, 10);
}

static void test_json(void)
{
    const char *js = "{\"type\": \"text\", \"text\":\"\\u0421\\u0435\\u0431 \\\"ok\\\"\\n\", \"t\": 1.25, \"e\":\"\\ud83d\\ude00!\"}";
    char o[64];
    TEST_ASSERT_TRUE(seb_json_str(js, "type", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("text", o);
    TEST_ASSERT_TRUE(seb_json_str(js, "text", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("Себ \"ok\" ", o);
    float t = 0;
    TEST_ASSERT_TRUE(seb_json_num(js, "t", &t));
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.25f, t);
    TEST_ASSERT_TRUE(seb_json_str(js, "e", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("?!", o);
    TEST_ASSERT_FALSE(seb_json_str(js, "nope", o, sizeof(o)));
    char small[4];
    TEST_ASSERT_TRUE(seb_json_str(js, "type", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("tex", small);
    char esc[32];
    seb_json_escape("a\"b\\c\n", esc, sizeof(esc));
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c ", esc);
}

static void test_linebuf(void)
{
    char buf[8];
    seb_linebuf_t lb;
    seb_linebuf_init(&lb, buf, sizeof(buf));
    const char *in = "ab\r\nlonglongline\nx\n";
    int lines = 0;
    for (const char *p = in; *p; p++) {
        if (seb_linebuf_put(&lb, *p)) {
            lines++;
            if (lines == 1) { TEST_ASSERT_EQUAL_STRING("ab", lb.buf); TEST_ASSERT_FALSE(lb.overflow); }
            if (lines == 2) { TEST_ASSERT_EQUAL_STRING("longlon", lb.buf); TEST_ASSERT_TRUE(lb.overflow); }
            if (lines == 3) TEST_ASSERT_EQUAL_STRING("x", lb.buf);
            seb_linebuf_reset(&lb);
        }
    }
    TEST_ASSERT_EQUAL(3, lines);
}

static void test_translit_wake(void)
{
    char o[64];
    seb_translit("Себастьян, ёлка «щи»", o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("Sebast'yan, yolka \"schi\"", o);
    char cmd[64];
    TEST_ASSERT_TRUE(seb_wake_split("Себастьян, включи свет.", cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("включи свет", cmd);
    TEST_ASSERT_TRUE(seb_wake_split("Себастьян.", cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    TEST_ASSERT_TRUE(seb_wake_split("ну Севастьян, который час?", cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("ну который час", cmd);
    TEST_ASSERT_TRUE(seb_wake_split("SEBASTIAN turn on", cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("turn on", cmd);
    TEST_ASSERT_FALSE(seb_wake_split("включи свет", cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("включи свет", cmd);
    // как в Arduino v21: ключ «севас» (Whisper так слышит «Себастьян») ловит и «Севастополь»
    TEST_ASSERT_TRUE(seb_wake_split("Севастополь", cmd, sizeof(cmd)));
    TEST_ASSERT_FALSE(seb_wake_split("бастион себе", cmd, sizeof(cmd)));
}

// ---------- медиа ----------
static void test_wav(void)
{
    uint8_t w[44 + 8];
    seb_wav_header(w, 24000, 8);
    for (int i = 0; i < 8; i++) w[44 + i] = (uint8_t)i;
    seb_wav_info_t inf;
    TEST_ASSERT_TRUE(seb_wav_parse(w, sizeof(w), 16000, &inf));
    TEST_ASSERT_EQUAL(24000, inf.rate);
    TEST_ASSERT_EQUAL(1, inf.channels);
    TEST_ASSERT_EQUAL(8, inf.data_len);
    TEST_ASSERT_EQUAL_PTR(w + 44, inf.data);
    TEST_ASSERT_TRUE(seb_wav_parse(w, 50, 16000, &inf));      // обрезанный поток
    TEST_ASSERT_EQUAL(6, inf.data_len);
    uint8_t raw[4] = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(seb_wav_parse(raw, 4, 24000, &inf));     // без заголовка — сырой PCM
    TEST_ASSERT_EQUAL(24000, inf.rate);
}

static void test_bat(void)
{
    TEST_ASSERT_EQUAL(0, seb_bat_percent(2900));
    TEST_ASSERT_EQUAL(8, seb_bat_percent(3500));
    TEST_ASSERT_EQUAL(45, seb_bat_percent(3750));
    TEST_ASSERT_EQUAL(100, seb_bat_percent(4250));
}

static void test_tar(void)
{
    uint8_t h[512];
    seb_tar_header(h, "kws/pos/p_0001.wav", 64044, 1790000000u);
    TEST_ASSERT_EQUAL_STRING("kws/pos/p_0001.wav", (char *)h);
    TEST_ASSERT_EQUAL_MEMORY("00000175054", h + 124, 11);     // 64044 в восьмеричной
    unsigned sum = 0;
    uint8_t c[512];
    memcpy(c, h, 512);
    memset(c + 148, ' ', 8);
    for (int i = 0; i < 512; i++) sum += c[i];
    TEST_ASSERT_EQUAL(sum, strtoul((char *)h + 148, NULL, 8));
}

// ---------- DSP ----------
static void test_ring(void)
{
    int16_t b[8];
    seb_ring_t r;
    seb_ring_init(&r, b, 8);
    int16_t x[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    TEST_ASSERT_EQUAL(6, seb_ring_write(&r, x, 6));
    int16_t y[10];
    TEST_ASSERT_EQUAL(4, seb_ring_read(&r, y, 4));
    TEST_ASSERT_EQUAL(4, seb_ring_write(&r, x + 6, 4));      // через край массива
    TEST_ASSERT_EQUAL(6, seb_ring_fill(&r));
    const int16_t *p;
    uint32_t n = seb_ring_peek(&r, &p);
    TEST_ASSERT_EQUAL(4, n);                                 // 5,6,7,8 до конца массива
    TEST_ASSERT_EQUAL(5, p[0]);
    seb_ring_consume(&r, 3);
    TEST_ASSERT_EQUAL(3, seb_ring_read(&r, y, 10));
    TEST_ASSERT_EQUAL(8, y[0]);
    TEST_ASSERT_EQUAL(10, y[2]);
    TEST_ASSERT_EQUAL(0, seb_ring_fill(&r));
}

static void test_resampler(void)
{
    // синус 1 кГц 24 кГц -> 48 кГц: амплитуда и частота сохраняются
    const int N = 2400;
    int16_t *in = malloc(N * 2);
    for (int i = 0; i < N; i++) in[i] = (int16_t)(10000 * sinf(2 * (float)M_PI * 1000 * i / 24000));
    seb_rs_t rs;
    seb_rs_init(&rs, 1, 24000, 48000);
    float *out = malloc(2 * 2 * N * sizeof(float));
    int used = 0, total = 0, off = 0;
    while (off < N) {                                 // кормим кусками, как микшер
        int k = N - off < 100 ? N - off : 100;
        int u;
        int m = seb_rs_process(&rs, in + off, k, &u, out + 2 * total, 1000);
        total += m;
        off += u;
        used += u;
        if (!u && !m) break;
    }
    TEST_ASSERT_EQUAL(N, used);
    TEST_ASSERT_INT_WITHIN(4, 2 * N, total);
    // выход[j] ≈ вход в момент j/2 (с задержкой 0): сверяем середину
    float maxerr = 0;
    for (int j = 200; j < total - 10; j++) {
        float ref = 10000 * sinf(2 * (float)M_PI * 1000 * (j / 2.0f) / 24000);
        float e = fabsf(out[2 * j] - ref);
        if (e > maxerr) maxerr = e;
        TEST_ASSERT_EQUAL_FLOAT(out[2 * j], out[2 * j + 1]);   // моно -> оба канала
    }
    TEST_ASSERT_LESS_THAN(150, (int)maxerr);
    // 44.1 -> 48 стерео: число кадров
    seb_rs_init(&rs, 2, 44100, 48000);
    int16_t st[2 * 441] = {0};
    int u;
    int m = seb_rs_process(&rs, st, 441, &u, out, 10000);
    TEST_ASSERT_EQUAL(441, u);
    TEST_ASSERT_INT_WITHIN(3, 480, m);
    free(in);
    free(out);
}

static void test_limiter(void)
{
    seb_lim_t l;
    seb_lim_init(&l, 0.5f, 48000);
    float b[2 * 4800];
    for (int i = 0; i < 4800; i++) b[2 * i] = b[2 * i + 1] = 1.2f * sinf(2 * (float)M_PI * 200 * i / 48000);
    seb_lim_process(&l, b, 4800);
    float mx = 0;
    for (int i = 2400; i < 4800; i++) if (fabsf(b[2 * i]) > mx) mx = fabsf(b[2 * i]);
    TEST_ASSERT_LESS_THAN(0.62f * 1000, (int)(mx * 1000));   // устоялся около порога
    TEST_ASSERT_GREATER_THAN(0.45f * 1000, (int)(mx * 1000));
    for (int i = 0; i < 4800; i++) TEST_ASSERT_TRUE(fabsf(b[2 * i]) < 1.0f);
}

static void fill_noise(int16_t *x, int n, int amp)
{
    for (int i = 0; i < n; i++) x[i] = (int16_t)((rand() % (2 * amp + 1)) - amp);
}

static void test_vad(void)
{
    seb_vad_cfg_t c;
    seb_vad_default_cfg(&c);
    seb_vad_t v;
    seb_vad_init(&v, &c);
    int16_t f[320];
    srand(1);
    for (int i = 0; i < 100; i++) { fill_noise(f, 320, 50); TEST_ASSERT_EQUAL(SEB_VAD_NONE, seb_vad_frame(&v, f, 320, false)); }
    float nf = v.nf;
    TEST_ASSERT_TRUE(nf > 10 && nf < 60);
    // речь 1 с громче шума в 20 раз
    int ev, starts = 0, dones = 0;
    for (int i = 0; i < 50; i++) {
        for (int k = 0; k < 320; k++) f[k] = (int16_t)(1500 * sinf(2 * (float)M_PI * 300 * (i * 320 + k) / 16000));
        ev = seb_vad_frame(&v, f, 320, false);
        if (ev == SEB_VAD_START) starts++;
    }
    TEST_ASSERT_EQUAL(1, starts);
    for (int i = 0; i < 50; i++) {
        fill_noise(f, 320, 50);
        ev = seb_vad_frame(&v, f, 320, false);
        if (ev == SEB_VAD_DONE) dones++;
    }
    TEST_ASSERT_EQUAL(1, dones);
    // щелчок 80 мс — SHORT
    int shorts = 0;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 320; k++) f[k] = (int16_t)(3000 * sinf(2 * (float)M_PI * 300 * k / 16000));
        seb_vad_frame(&v, f, 320, false);
    }
    for (int i = 0; i < 50; i++) { fill_noise(f, 320, 50); if (seb_vad_frame(&v, f, 320, false) == SEB_VAD_SHORT) shorts++; }
    TEST_ASSERT_EQUAL(1, shorts);
}

// ---------- радио ----------
static void test_rc_roundtrip(void)
{
    uint16_t hi[30], lo[30];
    const uint32_t code = 1382424;
    int n = seb_rc_tx_encode(code, 24, 389, hi, lo, 30);
    TEST_ASSERT_EQUAL(25, n);
    TEST_ASSERT_EQUAL(389 * 31, lo[24]);
    seb_rc_rx_t rx;
    seb_rc_rx_init(&rx);
    seb_rc_code_t out;
    int got = 0;
    for (int rep = 0; rep < 8; rep++) {       // 8 повторов, как шлёт передатчик (с дрожанием ±30 мкс)
        for (int i = 0; i < n; i++) {
            int j1 = (rand() % 61) - 30, j2 = (rand() % 61) - 30;
            if (seb_rc_rx_edge(&rx, hi[i] + j1, &out)) got++;
            if (seb_rc_rx_edge(&rx, lo[i] + j2, &out)) got++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL(3, got);
    TEST_ASSERT_EQUAL_UINT32(code, out.code);
    TEST_ASSERT_EQUAL(24, out.bits);
    TEST_ASSERT_INT_WITHIN(20, 389, out.delay_us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_old_and_new);
    RUN_TEST(test_events);
    RUN_TEST(test_head_cmds);
    RUN_TEST(test_route_and_alink_lines);
    RUN_TEST(test_alink);
    RUN_TEST(test_b64);
    RUN_TEST(test_json);
    RUN_TEST(test_linebuf);
    RUN_TEST(test_translit_wake);
    RUN_TEST(test_wav);
    RUN_TEST(test_bat);
    RUN_TEST(test_tar);
    RUN_TEST(test_ring);
    RUN_TEST(test_resampler);
    RUN_TEST(test_limiter);
    RUN_TEST(test_vad);
    RUN_TEST(test_rc_roundtrip);
    return UNITY_END();
}
