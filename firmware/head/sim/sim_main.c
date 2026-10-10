// Симулятор экрана головы на ПК: рисует все страницы и окна в PNG (sim/out/*.ppm -> png).
// Сборка и запуск: firmware/head/sim/run.sh
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "state.h"
#include "settings.h"
#include "ui.h"

extern int64_t sim_now;
static uint16_t fb[240 * 320];
static uint16_t buf[240 * 320];

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) fb[y * 240 + x] = *p++;
    lv_display_flush_ready(d);
}

static uint32_t tick(void) { return (uint32_t)sim_now; }

static void dump(const char *name)
{
    char fn[128];
    snprintf(fn, sizeof(fn), "out/%s.ppm", name);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n240 320\n255\n");
    for (int i = 0; i < 240 * 320; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(int ms)
{
    for (int i = 0; i < ms / 10; i++) { sim_now += 10; lv_timer_handler(); }
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);

    // правдоподобное состояние
    strcpy(g_set.wifi_ssid, "Keenetic-1234");
    strcpy(g_set.srv_host, "192.168.1.201");
    g_set.srv_port = 9010;
    g_set.volume = 0.30f;
    g_set.spk_ceil = 0.35f;
    g_set.out_mode = OUT_BOTH;
    g_set.wake_on = true;
    g_set.aec_on = true;
    g_st.head_ota_pct = -1;
    g_st.cp_bat = 3912; g_st.cp_bt = 2; g_st.cp_up = 754; g_st.cp_t2 = 1; g_st.cp_music = 0;
    g_st.rx_lines = 3770; g_st.ping_sent = 754; g_st.last_ack = 754;
    g_st.aux_plug = true; g_st.mic_ok = true; g_st.mic_level = 2400;
    strcpy(g_st.rf_rx, "RF RX: ТРИГГЕР 3"); g_st.rf_rx_ms = 99000;
    strcpy(g_st.rf_tx, "#1 ON ok");
    strcpy(g_st.v_srv, "Сервер: OK 18 мс  qwen2.5:7b");
    strcpy(g_st.v_state, "ГОТОВО: кусков 3, пауз 0"); g_st.v_state_col = COL_GREEN;
    strcpy(g_st.v_you, "Себастьян, какая завтра погода в Москве?");
    strcpy(g_st.v_seb, "Завтра в Москве облачно, около плюс двенадцати, после обеда возможен небольшой дождь. Зонт лучше взять с собой, сэр.");
    strcpy(g_st.v_heard, "слышал: Себастьян, какая завтра погода в Москве?");
    strcpy(g_st.v_t1, "ЗАП 3.1 | 1-й ЗВУК 4.8 | ВСЁ 11.2");
    strcpy(g_st.v_t2, "stt 0.9 мозг 2.1 звук 1.4 отпр 0.2");
    g_st.ls_running = true; g_st.ls_nf = 210; g_st.ls_lvl = 260; g_st.ls_thr = 630;
    g_st.kws_pos = 37; g_st.kws_neg = 12; g_st.kws_bg = 2;
    strcpy(g_st.kws_state, "POS: скажи после «пик»"); g_st.kws_state_col = COL_CYAN;
    strcpy(g_st.kws_info, "pos/p_0037.wav  пик 9120  ok");
    strcpy(g_st.kws_big, "СЕБАСТЬЯН"); strcpy(g_st.kws_small, "после «пик» — 3/10  (тап = стоп)"); g_st.kws_prompt_col = COL_GREEN;
    g_st.aec_ok = true; g_st.aec_erle_db = 18; g_st.aec_delay_ms = 22;
    strcpy(g_st.cp_ver, "c-idf-0.1");

    ui_init();
    static const char *N[8] = {"1_home", "2_voice", "3_train", "4_audio", "5_output", "6_sensors", "7_radio", "8_system"};
    for (int p = 0; p < 8; p++) { ui_goto(p); run(400); dump(N[p]); }
    ui_goto(0);
    st_flash(COL_ORANGE, "RF ТРИГГЕР 3"); run(300); dump("9_flash");
    g_st.flash_until = 0;
    g_st.head_ota_pct = 47; run(300); dump("10_ota"); g_st.head_ota_pct = -1;
    g_st.grace_mv = 3412; g_st.grace_until = sim_now + 3500; run(300); dump("11_grace"); g_st.grace_until = 0;
    g_st.bat_dead = true; g_st.bat_dead_mv = 2987; run(300); dump("12_bat_dead");
    printf("ok\n");
    return 0;
}
