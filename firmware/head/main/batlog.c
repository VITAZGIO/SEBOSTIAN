#include "batlog.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "state.h"
#include "settings.h"
#include "sdcard.h"
#include "net.h"
#include "mp3.h"
#include "voice.h"
#include "sysmon.h"
#include "seb_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BAT_NONE 2500

static char s_buf[4096];
static size_t s_len;
static char s_file[64];
static int64_t s_t_flush;
static SemaphoreHandle_t s_mx;

static void lock(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mx, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mx); }

static void flush_locked(void)
{
    if (!sdcard_ok() || !s_len || !s_file[0]) return;
    bool is_new = !sd_exists(s_file);
    FILE *f = fopen(s_file, "a");
    if (f) {
        if (is_new) fputs("time,mV,pct,mp3,bt,voice,vol\n", f);
        fwrite(s_buf, 1, s_len, f);
        fclose(f);
    }
    s_len = 0;
}

void batlog_flush(void)
{
    lock();
    flush_locked();
    unlock();
}

void batlog_tick(void)
{
    if (!sdcard_ok()) return;
    if (!link_rx_ok() || g_st.cp_bat < BAT_NONE) return;        // нет связи / нет аккума (стенд от USB)
    int mv = g_st.cp_bat, p = seb_bat_percent(mv);
    lock();
    if (p > 30 && p < 70) { flush_locked(); unlock(); return; } // середину не пишем
    char fn[64], ts[16];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (net_time_valid()) {
        strftime(fn, sizeof(fn), SD_MOUNT "/batlog/%Y%m%d_%H.csv", &tm);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    } else {
        unsigned long sec = (unsigned long)(now_ms() / 1000);
        snprintf(fn, sizeof(fn), SD_MOUNT "/batlog/notime_b%lu_%02lu.csv", (unsigned long)sysmon_boot_count(), sec / 3600);
        snprintf(ts, sizeof(ts), "+%lu", sec);
    }
    if (strcmp(s_file, fn)) {                                    // новый час — новый файл
        flush_locked();
        strcpy(s_file, fn);
        sd_mkdir(SD_MOUNT "/batlog");
    }
    if (s_len + 80 < sizeof(s_buf)) {
        s_len += snprintf(s_buf + s_len, sizeof(s_buf) - s_len, "%s,%d,%d,%d,%d,%d,%.2f\n", ts, mv, p, mp3_playing() ? 1 : 0,
                          g_st.cp_bt, voice_busy() ? 1 : 0, g_set.volume);
        g_st.batlog_lines++;
    }
    if (now_ms() - s_t_flush > 10000) { s_t_flush = now_ms(); flush_locked(); }
    unlock();
}
