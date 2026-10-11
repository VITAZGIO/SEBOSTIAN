#include "btaudio.h"
#include <string.h>
#include <math.h>
#include "board.h"
#include "cp.h"
#include "alink.h"
#include "seb_util.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

static const char *TAG = "bt";
#define BT_NAME     "Sebastian"
#define RB_SIZE     (24 * 1024)
#define RB_PREFETCH (12 * 1024)       // ~70 мс: копим перед стартом, чтобы рывки пакетов BT не давали заиканий
#define BLK         256              // стерео-кадров в блоке вывода

static i2s_chan_handle_t s_tx;
static RingbufHandle_t s_rb;
static volatile int s_rate = 44100, s_rate_cur = 44100;
static volatile int s_state;          // 0 ждёт, 1 подключён, 2 играет
static volatile bool s_bt_on;
static volatile bool s_beep_req, s_music;
static bool s_bt_flowing;
static char s_peer[32];
// маршруты от головы (AROUTE). До первой команды — как раньше: Bluetooth только в AUX.
static volatile bool s_bt_spk = false, s_bt_aux = true, s_head_aux = false;
static volatile bool s_routes_seen;

void bta_set_routes(bool bt_spk, bool bt_aux, bool head_aux)
{
    if (bt_spk != s_bt_spk || bt_aux != s_bt_aux || head_aux != s_head_aux)
        ESP_LOGI(TAG, "маршруты: BT -> дин %d, AUX %d; голова -> AUX %d", bt_spk, bt_aux, head_aux);
    s_bt_spk = bt_spk;
    s_bt_aux = bt_aux;
    s_head_aux = head_aux;
    s_routes_seen = true;
}
bool bta_routes_seen(void) { return s_routes_seen; }
int bta_rate(void) { return s_rate; }

int bta_bt_state(void) { return s_state; }
const char *bta_peer_name(void) { return s_peer; }
bool bta_music_on(void) { return s_music; }

void bta_beep(void) { ESP_LOGI(TAG, "мелодия в AUX"); s_beep_req = true; }

void bta_music_toggle(void)
{
    if (s_state == 2) { ESP_LOGI(TAG, "сейчас играет Bluetooth — мелодию не включаю"); return; }
    s_music = !s_music;
    ESP_LOGI(TAG, "тестовая музыка: %s", s_music ? "ВКЛ" : "выкл");
}

// ---------------- мелодии ----------------
typedef struct { float f; int ms; } note_t;
#define E4 329.6f
#define F4 349.2f
#define G4 392.0f
#define C4 261.6f
#define D4 293.7f
#define G3 196.0f
static const note_t SONG[] = {      // «Ода к радости», по кругу
    {E4, 400}, {E4, 400}, {F4, 400}, {G4, 400}, {G4, 400}, {F4, 400}, {E4, 400}, {D4, 400},
    {C4, 400}, {C4, 400}, {D4, 400}, {E4, 400}, {E4, 600}, {D4, 200}, {D4, 800},
    {E4, 400}, {E4, 400}, {F4, 400}, {G4, 400}, {G4, 400}, {F4, 400}, {E4, 400}, {D4, 400},
    {C4, 400}, {C4, 400}, {D4, 400}, {E4, 400}, {D4, 600}, {C4, 200}, {C4, 800},
    {D4, 400}, {D4, 400}, {E4, 400}, {C4, 400}, {D4, 400}, {E4, 200}, {F4, 200}, {E4, 400}, {C4, 400},
    {D4, 400}, {E4, 200}, {F4, 200}, {E4, 400}, {D4, 400}, {C4, 400}, {D4, 400}, {G3, 800}, {0, 400}};
static const int SONG_LEN = sizeof(SONG) / sizeof(SONG[0]);
static const float BEEP_NOTES[] = {523.3f, 659.3f, 784.0f, 1046.5f, 0};

static void set_rate(int rate)
{
    if (rate == s_rate_cur) return;
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
    s_rate_cur = rate;
    ESP_LOGI(TAG, "I2S -> %d Гц", rate);
}

// ---------------- микшер AUX ----------------
// Сумма: писк + Bluetooth (если маршрут BT -> AUX; приглушается, пока говорит голова) +
// тестовая мелодия + звук головы по аудио-связи (48 кГц -> частота I2S, с подстройкой дрейфа часов).
static seb_rs_t s_hrs;
static int s_hrs_rate;
static bool s_head_flow;

static int head_pull(float *out, int frames, int rate)
{
    seb_ring_t *r = cpal_head_ring();
    if (!r->buf) return 0;
    if (rate != s_hrs_rate) { seb_rs_init(&s_hrs, 2, 48000, rate); s_hrs_rate = rate; s_head_flow = false; }
    uint32_t fill = seb_ring_fill(r) / 2, target = 48000 / 25;      // держим ~40 мс
    if (!s_head_flow) {
        if (fill < target) return 0;
        s_head_flow = true;
    }
    float err = ((float)fill - target) / target;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    s_hrs.step = 48000.0 / rate * (1.0 + 0.003 * err);
    int made = 0;
    while (made < frames) {
        const int16_t *p;
        uint32_t n = seb_ring_peek(r, &p);
        int inf = (int)(n / 2);
        if (inf <= 0) break;
        int used = 0;
        int m = seb_rs_process(&s_hrs, p, inf, &used, out + 2 * made, frames - made);
        seb_ring_consume(r, used * 2);
        made += m;
        if (!m && !used) break;
    }
    if (made < frames) s_head_flow = false;
    return made;
}

static void out_task(void *arg)
{
    static int16_t buf[BLK * 2], bt[BLK * 2];
    static float acc[BLK * 2], hd[BLK * 2];
    int beep_i = -1, beep_k = 0, note_i = 0, note_k = 0;
    float ph = 0, mph = 0, duck = 1.0f;
    for (;;) {
        size_t wr;
        set_rate(s_state >= 1 ? s_rate : 44100);
        const int SR = s_rate_cur;
        memset(acc, 0, sizeof(acc));
        // 1) писк (BEEP / старт)
        if (s_beep_req) { s_beep_req = false; beep_i = 0; beep_k = 0; ph = 0; }
        if (beep_i >= 0) {
            const int fade = SR / 200;
            for (int i = 0; i < BLK && beep_i >= 0; i++) {
                float f = BEEP_NOTES[beep_i];
                int total = SR * (f > 0 ? 140 : 30) / 1000;
                float env = beep_k < fade ? (float)beep_k / fade : (beep_k > total - fade ? (float)(total - beep_k) / fade : 1.0f);
                float v = f > 0 ? 7000 * env * sinf(ph) : 0;
                ph += 2.0f * (float)M_PI * f / SR;
                if (ph > 2 * (float)M_PI) ph -= 2 * (float)M_PI;
                acc[2 * i] += v;
                acc[2 * i + 1] += v;
                if (++beep_k >= total) { beep_k = 0; ph = 0; if (++beep_i >= 5) beep_i = -1; }
            }
        }
        // 2) Bluetooth в AUX (с предзагрузкой ~70 мс, чтобы рывки пакетов BT не давали заиканий)
        size_t fill = RB_SIZE - xRingbufferGetCurFreeSize(s_rb);
        if (!s_bt_aux) {                                    // BT не в AUX — старое из кольца выбросить
            s_bt_flowing = false;
            size_t g;
            void *q;
            while ((q = xRingbufferReceiveUpTo(s_rb, &g, 0, RB_SIZE))) vRingbufferReturnItem(s_rb, q);
        }
        else if (!s_bt_flowing && fill >= RB_PREFETCH) s_bt_flowing = true;
        float dt = cpal_duck() ? 0.25f : 1.0f;
        if (s_bt_flowing) {
            size_t have = 0;
            while (have < sizeof(bt)) {
                size_t got = 0;
                void *p = xRingbufferReceiveUpTo(s_rb, &got, pdMS_TO_TICKS(20), sizeof(bt) - have);
                if (!p) break;
                memcpy((uint8_t *)bt + have, p, got);
                vRingbufferReturnItem(s_rb, p);
                have += got;
            }
            if (have < sizeof(bt)) s_bt_flowing = false;      // кольцо опустело — снова копим
            int nf = (int)(have / 4);
            for (int i = 0; i < nf; i++) {
                duck += (dt - duck) * 0.002f;
                acc[2 * i] += bt[2 * i] * duck;
                acc[2 * i + 1] += bt[2 * i + 1] * duck;
            }
        }
        // 3) тестовая мелодия (когда BT не играет)
        if (s_music && s_state == 2) { s_music = false; ESP_LOGI(TAG, "пошёл Bluetooth — мелодия выкл"); }
        if (s_music) {
            for (int i = 0; i < BLK; i++) {
                const note_t *n = &SONG[note_i];
                int total = SR * n->ms / 1000, att = SR / 100, rel = SR / 25;
                float env = note_k < att ? (float)note_k / att : (note_k > total - rel ? (float)(total - note_k) / rel : 1.0f);
                if (env < 0) env = 0;
                float v = 0;
                if (n->f > 0) {
                    v = sinf(mph) + 0.35f * sinf(2 * mph) + 0.15f * sinf(3 * mph);    // чуть «органа»
                    mph += 2.0f * (float)M_PI * n->f / SR;
                    if (mph > 2 * (float)M_PI) mph -= 2 * (float)M_PI;
                }
                acc[2 * i] += 6000 * env * v;
                acc[2 * i + 1] += 6000 * env * v;
                if (++note_k >= total) { note_k = 0; mph = 0; note_i = (note_i + 1) % SONG_LEN; }
            }
        } else note_i = note_k = 0;
        // 4) звук головы (голос Себастьяна, MP3) по аудио-связи
        int nh = s_head_aux ? head_pull(hd, BLK, SR) : 0;
        if (!s_head_aux && cpal_head_ring()->buf) seb_ring_clear(cpal_head_ring());
        for (int i = 0; i < 2 * nh; i++) acc[i] += hd[i];
        // сумма -> 16 бит (такт идёт всегда — PCM5102A так спокойнее)
        for (int i = 0; i < 2 * BLK; i++) {
            float v = acc[i];
            buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        i2s_channel_write(s_tx, buf, sizeof(buf), &wr, portMAX_DELAY);
    }
}

void bta_init(void)
{
    s_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
    cc.dma_frame_num = 256;
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_PCM_BCK, .ws = PIN_PCM_LCK, .dout = PIN_PCM_DIN, .din = I2S_GPIO_UNUSED},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    xTaskCreatePinnedToCore(out_task, "bt_out", 4096, NULL, 15, NULL, 1);
    cpal_init();
    ESP_LOGI(TAG, "I2S0 -> PCM5102A (BCK %d, LCK %d, DIN %d)", PIN_PCM_BCK, PIN_PCM_LCK, PIN_PCM_DIN);
}

// ---------------- Bluetooth ----------------
static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) ESP_LOGI(TAG, "сопряжено с %s", p->auth_cmpl.device_name);
        else ESP_LOGW(TAG, "сопряжение не удалось (%d)", p->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:              // SSP «Just works» — без PIN
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {            // старые устройства без SSP — PIN 0000
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
    } break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            strncpy(s_peer, (const char *)p->read_rmt_name.rmt_name, sizeof(s_peer) - 1);
            ESP_LOGI(TAG, "телефон: %s", s_peer);
        }
        break;
    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p)
{
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_state = 1;
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            esp_bt_gap_read_remote_name(p->conn_stat.remote_bda);
            ESP_LOGI(TAG, "ПОДКЛЮЧЁН");
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_state = 0;
            s_peer[0] = 0;
            if (s_bt_on) esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(TAG, "отключён — жду подключения");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) { s_state = 2; ESP_LOGI(TAG, "ИГРАЕТ"); }
        else if (s_state == 2) { s_state = 1; ESP_LOGI(TAG, "пауза"); }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        if (p->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint8_t sf = p->audio_cfg.mcc.cie.sbc_info.samp_freq;
            int r = 16000;
            if (sf & ESP_A2D_SBC_CIE_SF_32K) r = 32000;
            else if (sf & ESP_A2D_SBC_CIE_SF_44K) r = 44100;
            else if (sf & ESP_A2D_SBC_CIE_SF_48K) r = 48000;
            s_rate = r;
            ESP_LOGI(TAG, "кодек SBC, %d Гц", r);
        }
        break;
    default:
        break;
    }
}

static void a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (!s_rb || !len) return;
    // кольцо полное — выбрасываем (лучше щелчок, чем растущая задержка)
    if (s_bt_aux) xRingbufferSend(s_rb, data, len, 0);
    if (s_bt_spk) cpal_bt_push(data, len);          // в динамики головы — по аудио-связи
}

static void avrc_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t *p) { (void)ev; (void)p; }

void bta_bt_start(void)
{
    esp_err_t e;
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);       // BLE не нужен — память на Classic
    esp_bt_controller_config_t bc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((e = esp_bt_controller_init(&bc)) != ESP_OK) { ESP_LOGE(TAG, "контроллер: %s", esp_err_to_name(e)); return; }
    if ((e = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) { ESP_LOGE(TAG, "контроллер вкл: %s", esp_err_to_name(e)); return; }
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((e = esp_bluedroid_init_with_cfg(&cfg)) != ESP_OK) { ESP_LOGE(TAG, "bluedroid: %s", esp_err_to_name(e)); return; }
    if ((e = esp_bluedroid_enable()) != ESP_OK) { ESP_LOGE(TAG, "bluedroid вкл: %s", esp_err_to_name(e)); return; }
    esp_bt_sp_param_t pt = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t io = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(pt, &io, sizeof(uint8_t));
    esp_bt_pin_code_t pin;
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);
    esp_bt_gap_set_device_name(BT_NAME);
    esp_bt_gap_register_callback(gap_cb);
    esp_avrc_ct_register_callback(avrc_cb);
    esp_avrc_ct_init();                                  // AVRCP — на будущее (пауза телефона)
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    if ((e = esp_a2d_sink_init()) != ESP_OK) { ESP_LOGE(TAG, "A2DP: %s", esp_err_to_name(e)); return; }
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    s_bt_on = true;
    ESP_LOGI(TAG, "Bluetooth-колонка \"%s\" запущена", BT_NAME);
}

void bta_bt_stop(void)
{
    if (!s_bt_on) return;
    s_bt_on = false;
    esp_a2d_sink_deinit();
    esp_avrc_ct_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);     // память BT — под Wi-Fi (до перезагрузки)
    s_state = 0;
    ESP_LOGW(TAG, "Bluetooth выключен, память освобождена");
}
