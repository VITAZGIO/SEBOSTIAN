// Заглушки модулей головы для симулятора экрана (ui.c + шрифты + LVGL на ПК).
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "state.h"
#include "settings.h"
#include "app.h"
#include "display.h"
#include "seb_util.h"
#include "esp_heap_caps.h"
#include "sysmon.h"
#include "sdcard.h"

seb_state_t g_st;
seb_settings_t g_set;
int64_t sim_now = 100000;
const char *RF_NAMES[21] = {"1 ON", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14",
                            "15", "16", "17", "18", "19", "20", "21"};
const char *KWS_TIPS[] = {"POS: 100+ раз. Обычно, тихо, громко"};
const int KWS_TIPS_N = 1;

void st_lock(void) {}
void st_unlock(void) {}
void st_set(char *dst, size_t cap, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(dst, cap, fmt, ap); va_end(ap); }
void st_copy(char *out, size_t cap, const char *src) { strncpy(out, src, cap - 1); out[cap - 1] = 0; }
void st_flash(uint32_t col, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_st.flash, sizeof(g_st.flash), fmt, ap); va_end(ap); g_st.flash_col = col; g_st.flash_until = sim_now + 2000; }
int64_t now_ms(void) { return sim_now; }
bool link_rx_ok(void) { return true; }
bool link_tx_ok(void) { return true; }
void app_post(app_cmd_t c, int arg) { (void)c; (void)arg; }
bool disp_lock(int t) { (void)t; return true; }
void disp_unlock(void) {}
bool audio_spk_on(void) { return true; }
bool audio_aux_on(void) { return g_st.aux_plug; }
bool mp3_playing(void) { return true; }
bool net_connected(void) { return true; }
bool net_ap_active(void) { return false; }
void net_ip_str(char *o, size_t c) { snprintf(o, c, "192.168.1.36"); }
int net_rssi(void) { return -58; }
bool net_time_valid(void) { return true; }
bool sysmon_bad_reset(void) { return false; }
const char *sysmon_reason_name(void) { return "POWER ON"; }
uint32_t sysmon_boot_count(void) { return 1; }
bool sysmon_safe_mode(void) { return false; }
bool sdcard_ok(void) { return true; }
uint32_t sdcard_size_mb(void) { return 15193; }
bool sd_exists(const char *p) { (void)p; return true; }
bool voice_busy(void) { return false; }
bool mic_aec_active(void) { return true; }
bool seb_ota_pending(void) { return false; }
size_t heap_caps_get_free_size(uint32_t c) { return c == (1 << 10) ? 7800000 : 182000; }
size_t heap_caps_get_minimum_free_size(uint32_t c) { (void)c; return 151000; }
