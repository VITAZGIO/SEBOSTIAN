#include "state.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

seb_state_t g_st;
static SemaphoreHandle_t s_mx;

void st_init(void)
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.cp_mux = -1;
    g_st.cp_ota = 0;
    g_st.head_ota_pct = -1;
    g_st.kws_tip = -1;
    g_st.v_state_col = COL_WHITE;
    g_st.kws_state_col = COL_WHITE;
    strcpy(g_st.rf_rx, "RF RX: ---");
    strcpy(g_st.v_srv, "Сервер: нажми PING");
    strcpy(g_st.v_state, "готов");
    strcpy(g_st.kws_state, "выбери: POS / NEG / FON");
    s_mx = xSemaphoreCreateRecursiveMutex();
}

void st_lock(void) { xSemaphoreTakeRecursive(s_mx, portMAX_DELAY); }
void st_unlock(void) { xSemaphoreGiveRecursive(s_mx); }

void st_set(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    st_lock();
    vsnprintf(dst, cap, fmt, ap);
    st_unlock();
    va_end(ap);
}

void st_copy(char *out, size_t cap, const char *src)
{
    st_lock();
    strncpy(out, src, cap - 1);
    out[cap - 1] = 0;
    st_unlock();
}

void st_flash(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    st_lock();
    vsnprintf(g_st.flash, sizeof(g_st.flash), fmt, ap);
    g_st.flash_col = col;
    g_st.flash_until = now_ms() + 2000;
    st_unlock();
    va_end(ap);
}

int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool link_rx_ok(void) { return g_st.last_rx_ms && now_ms() - g_st.last_rx_ms < 1500; }
bool link_tx_ok(void) { return g_st.last_ack_ms && now_ms() - g_st.last_ack_ms < 3000; }
