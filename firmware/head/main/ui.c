#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "display.h"
#include "state.h"
#include "settings.h"
#include "app.h"
#include "audio.h"
#include "mp3.h"
#include "net.h"
#include "sysmon.h"
#include "sdcard.h"
#include "train.h"
#include "voice.h"
#include "mic.h"
#include "seb_ota.h"
#include "seb_util.h"
#include "seb_version.h"
#include "alink.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

LV_FONT_DECLARE(seb_font_14);
LV_FONT_DECLARE(seb_font_16);
LV_FONT_DECLARE(seb_font_24);
LV_FONT_DECLARE(seb_font_clock);

#define F14 (&seb_font_14)
#define F16 (&seb_font_16)
#define F24 (&seb_font_24)

static const char *PG_NAMES[PG_COUNT] = {"ГЛАВНАЯ", "ГОЛОС", "ОБУЧЕНИЕ", "ЗВУК", "ВЫВОД", "ДАТЧИКИ", "РАДИО", "СИСТЕМА"};

enum { NAV_PREV = -1, NAV_NEXT = -2 };

typedef struct {
    int pg, x, y, w, h;
    int cmd, arg;
    const char *label;
    const lv_font_t *font;
    lv_obj_t *obj, *lbl;
} btn_t;

static btn_t s_btn[] = {
    // HOME
    {PG_HOME, 3, 240, 75, 40, CMD_VOLM, 0, "ТИШЕ", F16, NULL, NULL},
    {PG_HOME, 82, 240, 75, 40, CMD_PLAY, 0, "▶ / ■", F16, NULL, NULL},
    {PG_HOME, 161, 240, 75, 40, CMD_VOLP, 0, "ГРОМЧЕ", F16, NULL, NULL},
    // VOICE
    {PG_VOICE, 3, 234, 112, 48, CMD_ASK, 0, "СПРОСИТЬ\n(тап = стоп)", F14, NULL, NULL},
    {PG_VOICE, 118, 234, 66, 48, CMD_WAKE, 0, "СЛОВО", F14, NULL, NULL},
    {PG_VOICE, 187, 234, 50, 48, CMD_PING, 0, "ПИНГ", F14, NULL, NULL},
    // TRAIN
    {PG_TRAIN, 3, 178, 115, 32, CMD_KPOS, 0, "POS ×10", F16, NULL, NULL},
    {PG_TRAIN, 122, 178, 115, 32, CMD_KNEG, 0, "NEG ×10", F16, NULL, NULL},
    {PG_TRAIN, 3, 214, 115, 32, CMD_KBG, 0, "ФОН 30 с", F16, NULL, NULL},
    {PG_TRAIN, 122, 214, 115, 32, CMD_KPLAY, 0, "ИГРАТЬ", F16, NULL, NULL},
    {PG_TRAIN, 3, 250, 115, 32, CMD_KDEL, 0, "УДАЛИТЬ", F16, NULL, NULL},
    {PG_TRAIN, 122, 250, 115, 32, CMD_KTIP, 0, "СОВЕТ", F16, NULL, NULL},
    // AUDIO
    {PG_AUDIO, 3, 112, 115, 40, CMD_PLAY, 0, "▶/■ МУЗЫКА", F16, NULL, NULL},
    {PG_AUDIO, 122, 112, 115, 40, CMD_MIC_TEST, 0, "ТЕСТ МИКР.", F16, NULL, NULL},
    {PG_AUDIO, 3, 156, 115, 40, CMD_VOLM, 0, "ГРОМК. −", F16, NULL, NULL},
    {PG_AUDIO, 122, 156, 115, 40, CMD_VOLP, 0, "ГРОМК. +", F16, NULL, NULL},
    {PG_AUDIO, 3, 200, 115, 40, CMD_SPK_MUTE, 0, "МЬЮТ ДИН.", F16, NULL, NULL},
    {PG_AUDIO, 122, 200, 115, 40, CMD_AUX_MUTE, 0, "МЬЮТ AUX", F16, NULL, NULL},
    {PG_AUDIO, 3, 244, 115, 40, CMD_AUX_MUSIC, 0, "МЕЛОДИЯ AUX", F14, NULL, NULL},
    {PG_AUDIO, 122, 244, 115, 40, CMD_LR_TEST, 0, "ТЕСТ L/R", F16, NULL, NULL},
    // OUTPUT
    {PG_OUT, 3, 116, 56, 44, CMD_ROUTE, 0 + OUT_SPK, "ДИН", F16, NULL, NULL},
    {PG_OUT, 62, 116, 56, 44, CMD_ROUTE, 0 + OUT_AUX, "AUX", F16, NULL, NULL},
    {PG_OUT, 121, 116, 56, 44, CMD_ROUTE, 0 + OUT_BOTH, "ОБА", F16, NULL, NULL},
    {PG_OUT, 180, 116, 57, 44, CMD_ROUTE, 0 + OUT_AUTO, "АВТО", F16, NULL, NULL},
    {PG_OUT, 3, 190, 56, 44, CMD_ROUTE, 10 + OUT_SPK, "ДИН", F16, NULL, NULL},
    {PG_OUT, 62, 190, 56, 44, CMD_ROUTE, 10 + OUT_AUX, "AUX", F16, NULL, NULL},
    {PG_OUT, 121, 190, 56, 44, CMD_ROUTE, 10 + OUT_BOTH, "ОБА", F16, NULL, NULL},
    {PG_OUT, 180, 190, 57, 44, CMD_ROUTE, 10 + OUT_AUTO, "АВТО", F16, NULL, NULL},
    // SENSORS
    {PG_SENS, 3, 240, 115, 40, CMD_LED, 0, "ЛЕНТА ДАЛЕЕ", F14, NULL, NULL},
    {PG_SENS, 122, 240, 115, 40, CMD_BEEP, 0, "БИП (AUX)", F16, NULL, NULL},
    // SYSTEM
    {PG_SYS, 3, 198, 115, 38, CMD_CPOTA, 0, "OTA СОПРОЦ.", F14, NULL, NULL},
    {PG_SYS, 122, 198, 115, 38, CMD_REBOOT, 0, "ПЕРЕЗАГРУЗКА", F14, NULL, NULL},
    {PG_SYS, 3, 242, 115, 38, CMD_AEC, 0, "ЭХОПОДАВЛ.", F14, NULL, NULL},
    {PG_SYS, 122, 242, 115, 38, CMD_AEC, 1, "ШУМОДАВ", F14, NULL, NULL},
};
#define NBTN (sizeof(s_btn) / sizeof(s_btn[0]))

static btn_t s_rf[22];

static bool s_inited;
static lv_obj_t *s_page[PG_COUNT];
static lv_obj_t *s_lines[PG_COUNT][12];
static uint32_t s_line_col[PG_COUNT][12];
static lv_obj_t *s_hdr, *s_hdr_time, *s_hdr_name, *s_hdr_wifi, *s_hdr_flash;
static lv_obj_t *s_dots[PG_COUNT];
static lv_obj_t *s_clock, *s_secs;
static lv_obj_t *s_you, *s_seb, *s_heard;
static lv_obj_t *s_kbig, *s_ksmall;
static lv_obj_t *s_micbar;
static lv_obj_t *s_circ[4], *s_circ_lbl[4];
static lv_obj_t *s_ov_ota, *s_ota_bar, *s_ota_pct, *s_ota_title;
static lv_obj_t *s_ov_dead, *s_dead_mv;
static lv_obj_t *s_ov_grace, *s_grace_mv, *s_grace_cnt;
static volatile int s_req_page = -1;
static int s_cur_page = -1;
static uint32_t s_btn_col[NBTN + 22];

// ---------------- помощники ----------------
static lv_obj_t *mk_label(lv_obj_t *par, int x, int y, int w, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void set_col(lv_obj_t *l, uint32_t col)
{
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
}

// строка статуса №idx на странице (y = 24 + idx*17), перерисовка только при изменении
static void line(int pg, int idx, const char *txt, uint32_t col)
{
    lv_obj_t *l = s_lines[pg][idx];
    if (!l) {
        l = s_lines[pg][idx] = mk_label(s_page[pg], 2, 2 + idx * 17, 236, F14, col);
        s_line_col[pg][idx] = col;
    }
    set_text(l, txt);
    if (s_line_col[pg][idx] != col) { set_col(l, col); s_line_col[pg][idx] = col; }
}

static void obj_plain(lv_obj_t *o, uint32_t bg)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void btn_cb(lv_event_t *e);

static void mk_btn(btn_t *b, lv_obj_t *par)
{
    lv_obj_t *o = lv_button_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, b->x, b->y - (par == lv_screen_active() ? 0 : 22));
    lv_obj_set_size(o, b->w, b->h);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xA0A0A0), 0);
    lv_obj_add_event_cb(o, btn_cb, LV_EVENT_CLICKED, b);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_set_style_text_font(l, b->font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, b->label);
    lv_obj_center(l);
    b->obj = o;
    b->lbl = l;
}

static void show_page(int p)
{
    p = (p + PG_COUNT) % PG_COUNT;
    for (int i = 0; i < PG_COUNT; i++) {
        if (i == p) lv_obj_remove_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == p ? COL_WHITE : 0x000000), 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_hex(i == p ? COL_WHITE : 0x606060), 0);
    }
    s_cur_page = p;
    g_st.page = p;
    if (p == PG_TRAIN) app_post(CMD_NONE, 2);     // пересчитать сэмплы на SD (в задаче app)
    ESP_LOGI("ui", "страница %s", PG_NAMES[p]);
}

static void btn_cb(lv_event_t *e)
{
    btn_t *b = lv_event_get_user_data(e);
    if (b->cmd == NAV_PREV) { show_page(s_cur_page - 1); return; }
    if (b->cmd == NAV_NEXT) { show_page(s_cur_page + 1); return; }
    app_post((app_cmd_t)b->cmd, b->arg);
}

static void grace_cb(lv_event_t *e) { g_st.grace_forced = true; }

void ui_goto(int page) { s_req_page = page; }

// ---------------- цвета кнопок ----------------
static uint32_t btn_color(const btn_t *b)
{
    switch (b->cmd) {
    case NAV_PREV: case NAV_NEXT: return COL_NAVY;
    case CMD_PLAY: return mp3_playing() ? COL_DKGREEN : COL_BTN;
    case CMD_SPK_MUTE: return g_st.spk_muted ? 0xB00000 : COL_BTN;
    case CMD_AUX_MUTE: return g_st.aux_muted ? 0xB00000 : (g_st.aux_plug ? COL_BTN : 0x202020);
    case CMD_AUX_MUSIC: return g_st.cp_music ? COL_DKGREEN : COL_BTN;
    case CMD_CPOTA: return (g_st.cp_ota >= 1 && g_st.cp_ota <= 3) ? COL_DKGREEN : COL_BTN;
    case CMD_REBOOT: return COL_DKRED;
    case CMD_ASK: return voice_busy() ? COL_ORANGE : COL_DKGREEN;
    case CMD_WAKE: return g_set.wake_on ? COL_DKGREEN : 0x404040;
    case CMD_AEC: return (b->arg ? g_set.ns_on : g_set.aec_on) ? COL_DKGREEN : 0x404040;
    case CMD_ROUTE: return (b->arg / 10 ? g_set.route_bt : g_set.route_head) == b->arg % 10 ? COL_DKGREEN : COL_BTN;
    case CMD_RF: return b->arg == 22 ? COL_DKRED : COL_BTN;
    default: return COL_BTN;
    }
}

static void refresh_btn(btn_t *b, uint32_t *cache)
{
    if (!b->obj) return;
    uint32_t c = btn_color(b);
    if (*cache != c) {
        *cache = c;
        lv_obj_set_style_bg_color(b->obj, lv_color_hex(c), 0);
    }
    if (b->cmd == CMD_WAKE) set_text(b->lbl, g_set.wake_on ? "СЛОВО\nвкл" : "СЛОВО\nвыкл");
    if (b->cmd == CMD_AEC && !b->arg) set_text(b->lbl, g_set.aec_on ? "ЭХОПОДАВЛ.\nвкл" : "ЭХОПОДАВЛ.\nвыкл");
    if (b->cmd == CMD_AEC && b->arg) set_text(b->lbl, g_set.ns_on ? "ШУМОДАВ\nвкл" : "ШУМОДАВ\nвыкл");
}

// ---------------- тексты ----------------
static void uptime_str(char *o, size_t cap, unsigned long sec)
{
    if (sec < 3600) snprintf(o, cap, "%luм %02luс", sec / 60, sec % 60);
    else if (sec < 86400) snprintf(o, cap, "%luч %02luм", sec / 3600, (sec / 60) % 60);
    else snprintf(o, cap, "%luд %02luч", sec / 86400, (sec / 3600) % 24);
}

static void bat_text(char *o, size_t cap, uint32_t *col)
{
    int b = g_st.cp_bat;
    *col = COL_WHITE;
    if (!link_rx_ok()) snprintf(o, cap, "АККУМ: ---");
    else if (b < 2500) snprintf(o, cap, "АККУМ: нет (питание от USB)");
    else {
        snprintf(o, cap, "АККУМ: %.2f В  %d%%", b / 1000.0f, seb_bat_percent(b));
        *col = b < 3300 ? COL_RED : b < 3600 ? COL_YELLOW : COL_GREEN;
    }
}

static const char *bt_text(void)
{
    if (!link_rx_ok()) return "---";
    return g_st.cp_bt == 2 ? "ИГРАЕТ" : g_st.cp_bt == 1 ? "ПОДКЛЮЧЁН" : "ждёт";
}

static void page_home(void)
{
    char b[96], c[16];
    uint32_t col;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    bool ok = net_time_valid();
    if (ok) strftime(c, sizeof(c), "%H:%M", &tm);
    else strcpy(c, "--:--");
    set_text(s_clock, c);
    set_col(s_clock, ok ? COL_WHITE : COL_GREY);
    if (ok) strftime(c, sizeof(c), "%S", &tm);
    else c[0] = 0;
    set_text(s_secs, c);
    static const char *DN[7] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
    if (ok) snprintf(b, sizeof(b), "%s  %02d.%02d.%04d", DN[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    else snprintf(b, sizeof(b), "нет времени (Wi-Fi?)");
    line(PG_HOME, 4, b, COL_CYAN);
    char ip[32];
    net_ip_str(ip, sizeof(ip));
    if (net_connected()) snprintf(b, sizeof(b), "Wi-Fi: %s  %d dBm", ip, net_rssi());
    else if (net_ap_active()) snprintf(b, sizeof(b), "Wi-Fi: настройка — точка «Sebastian-setup»");
    else snprintf(b, sizeof(b), "Wi-Fi: нет (%s)", g_set.wifi_ssid);
    line(PG_HOME, 6, b, net_connected() ? COL_GREEN : COL_RED);
    bat_text(b, sizeof(b), &col);
    line(PG_HOME, 7, b, col);
    bool lk = link_rx_ok() && link_tx_ok();
    snprintf(b, sizeof(b), "BT: %s   СВЯЗЬ: %s", bt_text(), lk ? "OK" : "НЕТ");
    line(PG_HOME, 8, b, lk ? COL_WHITE : COL_RED);
    snprintf(b, sizeof(b), "ЗВУК: %s  громк. %.2f", mp3_playing() ? "ИГРАЕТ" : "стоп", g_set.volume);
    line(PG_HOME, 9, b, mp3_playing() ? COL_GREEN : COL_WHITE);
    st_copy(b, sizeof(b), g_st.rf_rx);
    line(PG_HOME, 10, b, g_st.rf_rx_ms && now_ms() - g_st.rf_rx_ms < 3000 ? COL_YELLOW : COL_GREY);
}

static void page_voice(void)
{
    char b[640];
    st_copy(b, sizeof(b), g_st.v_srv);
    line(PG_VOICE, 0, b, !strncmp(b, "Сервер: OK", 15) ? COL_GREEN : strstr(b, "ОШИБ") ? COL_RED : COL_WHITE);
    st_copy(b, sizeof(b), g_st.v_state);
    line(PG_VOICE, 1, b, g_st.v_state_col);
    char t[660];
    st_copy(b, sizeof(b), g_st.v_you);
    snprintf(t, sizeof(t), "ТЫ: %s", b[0] ? b : "-");
    set_text(s_you, t);
    st_copy(b, sizeof(b), g_st.v_seb);
    snprintf(t, sizeof(t), "СЕБ: %s", b[0] ? b : "-");
    set_text(s_seb, t);
    st_copy(b, sizeof(b), g_st.v_heard);
    set_text(s_heard, b);
    st_copy(b, sizeof(b), g_st.v_t1);
    line(PG_VOICE, 8, b, COL_GREEN);
    st_copy(b, sizeof(b), g_st.v_t2);
    line(PG_VOICE, 9, b, COL_GREY);
    const char *ls;
    uint32_t lc = COL_WHITE;
    if (!g_set.wake_on) { ls = "СЛОВО: выкл"; lc = COL_GREY; }
    else if (!g_st.ls_running) { ls = sysmon_bad_reset() ? "СЛОВО: после сбоя — нажми СЛОВО" : "СЛОВО: старт через 20 с..."; lc = COL_GREY; }
    else if (g_st.ls_paused) { ls = "СЛОВО: пауза (звук/занят)"; lc = COL_GREY; }
    else if (g_st.ls_state == 1) { ls = "СЛОВО: РЕЧЬ... записываю"; lc = COL_RED; }
    else if (g_st.ls_state == 2) { ls = "СЛОВО: распознаю..."; lc = COL_YELLOW; }
    else if (g_st.ls_mode == 1) { ls = "СЛОВО: СЛУШАЮ КОМАНДУ"; lc = COL_CYAN; }
    else ls = mic_aec_active() ? "СЛОВО: жду «Себастьян» +эхо" : "СЛОВО: жду «Себастьян»";
    line(PG_VOICE, 10, ls, lc);
    if (g_st.ls_running) snprintf(b, sizeof(b), "шум %d  уровень %d  порог %d", (int)g_st.ls_nf, (int)g_st.ls_lvl, (int)g_st.ls_thr);
    else b[0] = 0;
    line(PG_VOICE, 11, b, COL_GREY);
}

static void page_train(void)
{
    char b[128];
    line(PG_TRAIN, 0, sdcard_ok() ? "SD: OK   слово: СЕБАСТЬЯН" : "SD: НЕТ КАРТЫ (FAT32)", sdcard_ok() ? COL_GREEN : COL_RED);
    snprintf(b, sizeof(b), "POS %d/100  NEG %d/60  ФОН %d/5", g_st.kws_pos, g_st.kws_neg, g_st.kws_bg);
    line(PG_TRAIN, 1, b, (g_st.kws_pos >= 100 && g_st.kws_neg >= 60 && g_st.kws_bg >= 5) ? COL_GREEN : COL_WHITE);
    st_copy(b, sizeof(b), g_st.kws_state);
    line(PG_TRAIN, 2, b, g_st.kws_state_col);
    st_copy(b, sizeof(b), g_st.kws_info);
    line(PG_TRAIN, 3, b, COL_GREY);
    st_copy(b, sizeof(b), g_st.kws_big);
    set_text(s_kbig, b);
    set_col(s_kbig, g_st.kws_prompt_col);
    st_copy(b, sizeof(b), g_st.kws_small);
    set_text(s_ksmall, b);
    char ip[32];
    net_ip_str(ip, sizeof(ip));
    if (net_connected()) snprintf(b, sizeof(b), "ПК: http://%s/kws.tar", ip);
    else snprintf(b, sizeof(b), "ПК: нет Wi-Fi");
    line(PG_TRAIN, 7, b, net_connected() ? COL_CYAN : COL_RED);
    int tip = g_st.kws_tip;
    line(PG_TRAIN, 8, tip >= 0 && tip < KWS_TIPS_N ? KWS_TIPS[tip] : "СОВЕТ — подсказки, как записывать", COL_GREY);
}

static void page_audio(void)
{
    char b[96];
    snprintf(b, sizeof(b), "МУЗЫКА: %s  гр %.2f/%.2f", mp3_playing() ? "ИГРАЕТ" : "стоп", g_set.volume, g_set.spk_ceil);
    line(PG_AUDIO, 0, b, mp3_playing() ? COL_GREEN : COL_WHITE);
    if (!sdcard_ok()) line(PG_AUDIO, 1, "SD: НЕТ (FAT32? CS=5)", COL_RED);
    else {
        bool f = sd_exists(MP3_FILE);
        snprintf(b, sizeof(b), "SD: %lu МБ  test.mp3: %s", (unsigned long)sdcard_size_mb(), f ? "ЕСТЬ" : "НЕТ");
        line(PG_AUDIO, 1, b, f ? COL_GREEN : COL_YELLOW);
    }
    snprintf(b, sizeof(b), "динамики %s, AUX %s, штекер %s", audio_spk_on() ? "ВКЛ" : "выкл",
             audio_aux_on() ? "ВКЛ" : "выкл", g_st.aux_plug ? "ДА" : "нет");
    line(PG_AUDIO, 2, b, COL_WHITE);
    if (!g_st.mic_ok) line(PG_AUDIO, 3, "МИКР: I2S ОШИБКА", COL_RED);
    else {
        snprintf(b, sizeof(b), "МИКР %5d", (int)g_st.mic_level);
        line(PG_AUDIO, 3, b, COL_CYAN);
        int w = (int)(g_st.mic_level / 40.0f);
        if (w > 150) w = 150;
        lv_bar_set_value(s_micbar, w, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_micbar, lv_color_hex(w > 120 ? COL_RED : COL_GREEN), LV_PART_INDICATOR);
    }
    snprintf(b, sizeof(b), "BT: %s", bt_text());
    line(PG_AUDIO, 4, b, g_st.cp_bt ? COL_CYAN : COL_WHITE);
}

static const char *route_text(bool spk, bool aux)
{
    return spk && aux ? "динамики + AUX" : aux ? "AUX" : spk ? "динамики" : "никуда";
}

static void page_out(void)
{
    char b[96];
    audio_route_t r;
    audio_routes(&r);
    snprintf(b, sizeof(b), "СЕБАСТЬЯН → %s%s", route_text(r.head_spk, r.head_aux), g_set.route_head == OUT_AUTO ? " (авто)" : "");
    line(PG_OUT, 0, b, COL_CYAN);
    snprintf(b, sizeof(b), "BLUETOOTH → %s%s  (%s)", route_text(r.bt_spk, r.bt_aux), g_set.route_bt == OUT_AUTO ? " (авто)" : "",
             bt_text());
    line(PG_OUT, 1, b, g_st.cp_bt == 2 ? COL_GREEN : COL_WHITE);
    if (g_st.spk_muted || g_st.aux_muted) line(PG_OUT, 2, "внимание: включён МЬЮТ на странице ЗВУК", COL_RED);
    else {
        snprintf(b, sizeof(b), "штекер AUX: %s", g_st.aux_plug ? "вставлен" : "нет");
        line(PG_OUT, 2, b, g_st.aux_plug ? COL_GREEN : COL_GREY);
    }
    // здоровье аудио-связи (провода 23->1 и 46->19)
    bool bt_wait = r.bt_spk && g_st.cp_bt == 2 && now_ms() - alink_bt_last_ms() > 1500;
    if (strncmp(g_st.cp_ver, "c-idf", 5)) line(PG_OUT, 3, "сопроц. Arduino: маршруты не работают", COL_YELLOW);
    else if (bt_wait) line(PG_OUT, 3, "НЕТ Bluetooth по проводу 23 → 1!", COL_RED);
    else {
        snprintf(b, sizeof(b), "провод: BT %lu ош%lu, AUX %lu ош%lu", (unsigned long)g_st.al_rx_ok,
                 (unsigned long)(g_st.al_rx_crc + g_st.al_rx_lost), (unsigned long)g_st.cp_al_ok,
                 (unsigned long)(g_st.cp_al_crc + g_st.cp_al_lost));
        line(PG_OUT, 3, b, COL_GREY);
    }
}

static void page_sens(void)
{
    char b[96], u[24];
    bool l = link_rx_ok();
    if (l) snprintf(b, sizeof(b), "ESP->S3: OK  %lu строк", (unsigned long)g_st.rx_lines);
    else if (g_st.rx_bytes) snprintf(b, sizeof(b), "ESP->S3: МУСОР %lu б (GND?)", (unsigned long)g_st.rx_bytes);
    else snprintf(b, sizeof(b), "ESP->S3: НЕТ (ESP 17 -> S3 18)");
    line(PG_SENS, 0, b, l ? COL_GREEN : COL_RED);
    if (link_tx_ok()) snprintf(b, sizeof(b), "S3->ESP: OK  ping %lu/%lu", (unsigned long)g_st.last_ack, (unsigned long)g_st.ping_sent);
    else snprintf(b, sizeof(b), "S3->ESP: НЕТ (S3 8 -> ESP 16)");
    line(PG_SENS, 1, b, link_tx_ok() ? COL_GREEN : COL_RED);
    uint32_t col;
    bat_text(b, sizeof(b), &col);
    line(PG_SENS, 2, b, col);
    uptime_str(u, sizeof(u), g_st.cp_up);
    snprintf(b, sizeof(b), "BT: %s   сопроц. %s", bt_text(), l ? u : "---");
    line(PG_SENS, 3, b, COL_WHITE);
    snprintf(b, sizeof(b), "ШТЕКЕР AUX: %s", g_st.aux_plug ? "вставлен" : "нет");
    line(PG_SENS, 4, b, g_st.aux_plug ? COL_GREEN : COL_WHITE);
    st_copy(b, sizeof(b), g_st.rf_rx);
    line(PG_SENS, 5, b, g_st.rf_rx_ms && now_ms() - g_st.rf_rx_ms < 3000 ? COL_YELLOW : COL_WHITE);
    int st[4] = {g_st.cp_t1, g_st.cp_t2, g_st.cp_t3, g_st.cp_mic};
    for (int i = 0; i < 4; i++) {
        bool on = l && st[i];
        uint32_t bg = on ? COL_GREEN : (l ? 0x202020 : 0x400000);
        lv_obj_set_style_bg_color(s_circ[i], lv_color_hex(bg), 0);
        set_col(s_circ_lbl[i], on ? 0x000000 : COL_WHITE);
    }
}

static void page_radio(void)
{
    char b[64], t[40];
    st_copy(b, sizeof(b), g_st.rf_rx);
    line(PG_RADIO, 0, b, g_st.rf_rx_ms && now_ms() - g_st.rf_rx_ms < 3000 ? COL_YELLOW : COL_WHITE);
    st_copy(t, sizeof(t), g_st.rf_tx);
    snprintf(b, sizeof(b), "RF TX: %s", t[0] ? t : "---");
    line(PG_RADIO, 1, b, COL_CYAN);
}

static void page_sys(void)
{
    char b[128], ip[32], u[24];
    bool wc = net_connected();
    if (wc) snprintf(b, sizeof(b), "Wi-Fi: %s  %d dBm", g_set.wifi_ssid, net_rssi());
    else snprintf(b, sizeof(b), "Wi-Fi: %s  НЕТ%s", g_set.wifi_ssid, net_ap_active() ? " (точка доступа вкл)" : "");
    line(PG_SYS, 0, b, wc ? COL_GREEN : COL_RED);
    net_ip_str(ip, sizeof(ip));
    snprintf(b, sizeof(b), "IP: %s", ip);
    line(PG_SYS, 1, b, wc ? COL_WHITE : COL_GREY);
    if (sysmon_safe_mode()) line(PG_SYS, 2, "OTA головы: БЕЗОПАСНЫЙ РЕЖИМ (3 сбоя)", COL_RED);
    else if (seb_ota_pending()) line(PG_SYS, 2, "OTA головы: новая прошивка на проверке", COL_YELLOW);
    else line(PG_SYS, 2, wc ? "OTA: sebastian-head.local/ota" : "OTA головы: жду Wi-Fi", wc ? COL_GREEN : COL_YELLOW);
    int s = g_st.cp_ota;
    st_copy(ip, sizeof(ip), g_st.cp_ip);
    switch (s) {
    case 0: snprintf(b, sizeof(b), "OTA сопроц.: выкл"); break;
    case 1: snprintf(b, sizeof(b), "OTA сопроц.: подключаюсь к Wi-Fi..."); break;
    case 2: snprintf(b, sizeof(b), "OTA сопроц.: ЖДУ %s", ip); break;
    case 3: snprintf(b, sizeof(b), "OTA сопроц.: прошивка %d%%", g_st.cp_ota_pct); break;
    case 4: snprintf(b, sizeof(b), "OTA сопроц.: готово, перезагрузка"); break;
    default: snprintf(b, sizeof(b), "OTA сопроц.: ОШИБКА"); break;
    }
    line(PG_SYS, 3, b, s == 2 ? COL_GREEN : s == 9 ? COL_RED : COL_WHITE);
    snprintf(b, sizeof(b), "RAM %uК / мин %uК  PSRAM %uК",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    line(PG_SYS, 4, b, COL_WHITE);
    uptime_str(u, sizeof(u), (unsigned long)(now_ms() / 1000));
    snprintf(b, sizeof(b), "аптайм: %s", u);
    line(PG_SYS, 5, b, COL_WHITE);
    snprintf(b, sizeof(b), "старт: %s  загрузка №%lu", sysmon_reason_name(), (unsigned long)sysmon_boot_count());
    line(PG_SYS, 6, b, sysmon_bad_reset() ? COL_RED : COL_WHITE);
    snprintf(b, sizeof(b), "%s  " __DATE__ "  лог акк. %lu", SEB_HEAD_VERSION, (unsigned long)g_st.batlog_lines);
    line(PG_SYS, 7, b, COL_GREY);
    st_copy(u, sizeof(u), g_st.cp_ver);
    snprintf(b, sizeof(b), "сопроцессор: %s", u[0] ? u : "Arduino (версию не шлёт)");
    line(PG_SYS, 8, b, COL_GREY);
    if (!g_st.aec_ok) snprintf(b, sizeof(b), "эхоподавление: не запустилось");
    else snprintf(b, sizeof(b), "эхо: %s, -%.0f дБ, задержка %.0f мс", g_set.aec_on ? "вкл" : "выкл", g_st.aec_erle_db, g_st.aec_delay_ms);
    line(PG_SYS, 9, b, COL_GREY);
}

// ---------------- обновление (каждые 150 мс в задаче LVGL) ----------------
static void refresh_cb(lv_timer_t *t)
{
    if (s_req_page >= 0) { show_page(s_req_page); s_req_page = -1; }
    int64_t now = now_ms();
    // шапка
    bool flash = now < g_st.flash_until;
    if (flash) {
        char b[64];
        st_copy(b, sizeof(b), g_st.flash);
        set_text(s_hdr_flash, b);
        lv_obj_set_style_bg_color(s_hdr, lv_color_hex(g_st.flash_col), 0);
        lv_obj_remove_flag(s_hdr_flash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hdr_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hdr_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hdr_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hdr_flash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hdr_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hdr_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hdr_wifi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_hdr, lv_color_hex(sysmon_bad_reset() ? COL_DKRED : COL_NAVY), 0);
        char c[16];
        if (net_time_valid()) {
            time_t tt = time(NULL);
            struct tm tm;
            localtime_r(&tt, &tm);
            strftime(c, sizeof(c), "%H:%M", &tm);
        } else strcpy(c, "--:--");
        set_text(s_hdr_time, c);
        set_text(s_hdr_name, PG_NAMES[s_cur_page]);
        const char *w;
        int r = net_rssi();
        if (g_st.head_ota_pct >= 0) w = "OTA";
        else if (net_connected()) w = r > -60 ? "WiFi+++" : r > -75 ? "WiFi++" : "WiFi+";
        else if (net_ap_active()) w = "AP";
        else w = "WiFi−";
        set_text(s_hdr_wifi, w);
        set_col(s_hdr_wifi, net_connected() ? COL_GREEN : COL_RED);
    }
    // полноэкранные окна
    if (g_st.head_ota_pct >= 0) {
        lv_obj_remove_flag(s_ov_ota, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_ota_bar, g_st.head_ota_pct, LV_ANIM_OFF);
        char b[16];
        snprintf(b, sizeof(b), "%d%%", g_st.head_ota_pct);
        set_text(s_ota_pct, b);
        set_text(s_ota_title, g_st.head_ota_pct >= 100 ? "ГОТОВО, ПЕРЕЗАГРУЗКА" : "ПРОШИВКА ПО WI-FI");
    } else lv_obj_add_flag(s_ov_ota, LV_OBJ_FLAG_HIDDEN);
    if (g_st.bat_dead) {
        lv_obj_remove_flag(s_ov_dead, LV_OBJ_FLAG_HIDDEN);
        char b[24];
        if (g_st.bat_dead_mv > 0) snprintf(b, sizeof(b), "%.2f В", g_st.bat_dead_mv / 1000.0f);
        else b[0] = 0;
        set_text(s_dead_mv, b);
    } else lv_obj_add_flag(s_ov_dead, LV_OBJ_FLAG_HIDDEN);
    if (g_st.grace_until > now) {
        lv_obj_remove_flag(s_ov_grace, LV_OBJ_FLAG_HIDDEN);
        char b[48];
        snprintf(b, sizeof(b), "%.2f В", g_st.grace_mv / 1000.0f);
        set_text(s_grace_mv, b);
        snprintf(b, sizeof(b), "иначе сплю через %d с", (int)((g_st.grace_until - now) / 1000) + 1);
        set_text(s_grace_cnt, b);
    } else lv_obj_add_flag(s_ov_grace, LV_OBJ_FLAG_HIDDEN);

    switch (s_cur_page) {
    case PG_HOME: page_home(); break;
    case PG_VOICE: page_voice(); break;
    case PG_TRAIN: page_train(); break;
    case PG_AUDIO: page_audio(); break;
    case PG_OUT: page_out(); break;
    case PG_SENS: page_sens(); break;
    case PG_RADIO: page_radio(); break;
    case PG_SYS: page_sys(); break;
    }
    for (size_t i = 0; i < NBTN; i++)
        if (s_btn[i].pg == s_cur_page || s_btn[i].pg < 0) refresh_btn(&s_btn[i], &s_btn_col[i]);
    if (s_cur_page == PG_RADIO)
        for (int i = 0; i < 22; i++) refresh_btn(&s_rf[i], &s_btn_col[NBTN + i]);
}

// ---------------- построение ----------------
static lv_obj_t *mk_overlay(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    obj_plain(o, 0x000000);
    lv_obj_set_size(o, 240, 320);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);         // глотает касания под собой
    return o;
}

static lv_obj_t *mk_center(lv_obj_t *par, int y, const lv_font_t *f, uint32_t col, const char *txt)
{
    lv_obj_t *l = mk_label(par, 0, y, 240, f, col);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, txt);
    return l;
}

static btn_t s_nav[2] = {
    {-1, 2, 286, 60, 32, NAV_PREV, 0, "<", F24, NULL, NULL},
    {-1, 178, 286, 60, 32, NAV_NEXT, 0, ">", F24, NULL, NULL},
};

void ui_init(void)
{
    if (s_inited) return;
    s_inited = true;
    disp_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    obj_plain(scr, 0x000000);

    // шапка
    s_hdr = lv_obj_create(scr);
    obj_plain(s_hdr, COL_NAVY);
    lv_obj_set_size(s_hdr, 240, 21);
    lv_obj_set_pos(s_hdr, 0, 0);
    s_hdr_time = mk_label(s_hdr, 4, 2, 0, F16, COL_WHITE);
    s_hdr_name = mk_label(s_hdr, 54, 2, 124, F16, COL_WHITE);
    lv_obj_set_style_text_align(s_hdr_name, LV_TEXT_ALIGN_CENTER, 0);
    s_hdr_wifi = mk_label(s_hdr, 178, 3, 58, F14, COL_GREEN);
    lv_obj_set_style_text_align(s_hdr_wifi, LV_TEXT_ALIGN_RIGHT, 0);
    s_hdr_flash = mk_label(s_hdr, 4, 2, 232, F16, COL_WHITE);
    lv_obj_add_flag(s_hdr_flash, LV_OBJ_FLAG_HIDDEN);

    // страницы (контейнеры под шапкой: y 22..284)
    for (int p = 0; p < PG_COUNT; p++) {
        s_page[p] = lv_obj_create(scr);
        obj_plain(s_page[p], 0x000000);
        lv_obj_set_size(s_page[p], 240, 262);
        lv_obj_set_pos(s_page[p], 0, 22);
        lv_obj_add_flag(s_page[p], LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < NBTN; i++) mk_btn(&s_btn[i], s_page[s_btn[i].pg]);
    for (int i = 0; i < 22; i++) {
        s_rf[i] = (btn_t){PG_RADIO, 2 + (i % 4) * 60, 62 + (i / 4) * 37, 56, 33, CMD_RF, i + 1, i < 21 ? RF_NAMES[i] : "HOLD", F14, NULL, NULL};
        mk_btn(&s_rf[i], s_page[PG_RADIO]);
    }

    // HOME: часы
    s_clock = mk_label(s_page[PG_HOME], 0, 4, 200, &seb_font_clock, COL_WHITE);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    s_secs = mk_label(s_page[PG_HOME], 196, 30, 40, F24, COL_WHITE);
    // VOICE: многострочные «ты» и «Себастьян»
    s_you = mk_label(s_page[PG_VOICE], 2, 2 + 2 * 17, 236, F14, COL_CYAN);
    lv_obj_set_height(s_you, 35);
    lv_obj_set_style_text_line_space(s_you, -1, 0);
    lv_label_set_long_mode(s_you, LV_LABEL_LONG_DOT);
    s_seb = mk_label(s_page[PG_VOICE], 2, 2 + 4 * 17, 236, F14, COL_YELLOW);
    lv_obj_set_height(s_seb, 52);
    lv_obj_set_style_text_line_space(s_seb, -1, 0);
    lv_label_set_long_mode(s_seb, LV_LABEL_LONG_DOT);
    s_heard = mk_label(s_page[PG_VOICE], 2, 2 + 7 * 17, 236, F14, COL_GREY);
    lv_obj_set_height(s_heard, 17);
    lv_label_set_long_mode(s_heard, LV_LABEL_LONG_DOT);
    // TRAIN: крупная подсказка
    s_kbig = mk_center(s_page[PG_TRAIN], 72, F24, COL_GREEN, "");
    s_ksmall = mk_center(s_page[PG_TRAIN], 104, F14, 0xC0C0C0, "");
    // AUDIO: полоска уровня микрофона
    s_micbar = lv_bar_create(s_page[PG_AUDIO]);
    lv_obj_set_pos(s_micbar, 80, 2 + 3 * 17 + 3);
    lv_obj_set_size(s_micbar, 150, 10);
    lv_bar_set_range(s_micbar, 0, 150);
    lv_obj_set_style_radius(s_micbar, 0, 0);
    lv_obj_set_style_radius(s_micbar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_micbar, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_micbar, LV_OPA_COVER, 0);
    // OUTPUT: подписи групп
    mk_label(s_page[PG_OUT], 3, 96 - 22, 234, F14, 0xC0C0C0);
    lv_label_set_text(lv_obj_get_child(s_page[PG_OUT], -1), "Себастьян, музыка с SD, звуки:");
    mk_label(s_page[PG_OUT], 3, 170 - 22, 234, F14, 0xC0C0C0);
    lv_label_set_text(lv_obj_get_child(s_page[PG_OUT], -1), "Bluetooth (телефон):");
    lv_obj_t *hint = mk_label(s_page[PG_OUT], 3, 240 - 22, 234, F14, COL_GREY);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "АВТО: штекер в AUX → AUX,\nнет штекера → динамики");

    // SENSORS: круги
    static const char *CN[4] = {"T1", "T2", "T3", "МИК"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_obj_create(s_page[PG_SENS]);
        obj_plain(c, 0x202020);
        lv_obj_set_size(c, 44, 44);
        lv_obj_set_pos(c, 8 + i * 60, 190 - 22 - 22);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0xA0A0A0), 0);
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_text_font(l, F14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
        lv_label_set_text(l, CN[i]);
        lv_obj_center(l);
        s_circ[i] = c;
        s_circ_lbl[i] = l;
    }

    // навигация
    for (int i = 0; i < 2; i++) mk_btn(&s_nav[i], scr);
    for (int i = 0; i < PG_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        obj_plain(d, 0x000000);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_pos(d, 120 - (PG_COUNT - 1) * 7 + i * 14 - 5, 297);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        s_dots[i] = d;
    }

    // окно OTA
    s_ov_ota = mk_overlay();
    s_ota_title = mk_center(s_ov_ota, 100, F24, COL_WHITE, "ПРОШИВКА ПО WI-FI");
    s_ota_bar = lv_bar_create(s_ov_ota);
    lv_obj_set_pos(s_ota_bar, 20, 160);
    lv_obj_set_size(s_ota_bar, 200, 20);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    s_ota_pct = mk_center(s_ov_ota, 195, F24, COL_WHITE, "0%");
    mk_center(s_ov_ota, 240, F14, COL_GREY, "не выключай питание");

    // окно «БАТАРЕЯ СЕЛА»
    s_ov_dead = mk_overlay();
    mk_center(s_ov_dead, 80, F24, COL_RED, "БАТАРЕЯ\nСЕЛА");
    s_dead_mv = mk_center(s_ov_dead, 150, F24, COL_WHITE, "");
    mk_center(s_ov_dead, 200, F16, COL_WHITE, "поставь на зарядку");
    mk_center(s_ov_dead, 225, F14, COL_GREY, "включусь сама от 3.55 В");

    // окно «ВКЛЮЧИТЬ» на севшем аккуме
    s_ov_grace = mk_overlay();
    mk_center(s_ov_grace, 30, F24, COL_RED, "БАТАРЕЯ СЕЛА");
    s_grace_mv = mk_center(s_ov_grace, 65, F24, COL_WHITE, "");
    mk_center(s_ov_grace, 105, F16, COL_WHITE, "зарядка подключена?");
    lv_obj_t *gb = lv_button_create(s_ov_grace);
    lv_obj_remove_style_all(gb);
    lv_obj_set_pos(gb, 20, 150);
    lv_obj_set_size(gb, 200, 90);
    lv_obj_set_style_radius(gb, 10, 0);
    lv_obj_set_style_bg_opa(gb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gb, lv_color_hex(COL_DKGREEN), 0);
    lv_obj_set_style_bg_color(gb, lv_color_hex(COL_ORANGE), LV_STATE_PRESSED);
    lv_obj_add_event_cb(gb, grace_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *gl = lv_label_create(gb);
    lv_obj_set_style_text_font(gl, F24, 0);
    lv_obj_set_style_text_color(gl, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(gl, "ВКЛЮЧИТЬ");
    lv_obj_center(gl);
    mk_center(s_ov_grace, 250, F14, COL_GREY, "или сенсор 2 на корпусе");
    s_grace_cnt = mk_center(s_ov_grace, 280, F16, COL_YELLOW, "");

    show_page(PG_HOME);
    lv_timer_create(refresh_cb, 150, NULL);
    refresh_cb(NULL);
    disp_unlock();
}
