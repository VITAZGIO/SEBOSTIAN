#include "link.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "board.h"
#include "state.h"
#include "settings.h"
#include "app.h"
#include "seb_proto.h"
#include "seb_util.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "link";
static SemaphoreHandle_t s_tx_mx;
static bool s_setup;

void link_uart_setup(void)
{
    if (s_setup) return;
    s_setup = true;
    s_tx_mx = xSemaphoreCreateMutex();
    uart_config_t c = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(LINK_UART, 2048, 1024, 0, NULL, 0);
    uart_param_config(LINK_UART, &c);
    uart_set_pin(LINK_UART, PIN_LINK_TX, PIN_LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void link_send(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
    b[n++] = '\n';
    xSemaphoreTake(s_tx_mx, portMAX_DELAY);
    uart_write_bytes(LINK_UART, b, n);
    xSemaphoreGive(s_tx_mx);
}

void link_send_cfg(void)
{
    char b[300];
    if (!settings_have_wifi()) return;
    if (seb_proto_fmt_cfg(b, sizeof(b), g_set.wifi_ssid, g_set.wifi_pass, g_set.ota_pass) > 0) {
        link_send("%s", b);
        ESP_LOGI(TAG, "со-процессору отправлены настройки Wi-Fi/OTA");
    }
}

bool link_wait_alive(int ms)
{
    char b[32];
    int n = 0;
    int64_t t0 = now_ms();
    while (now_ms() - t0 < ms) {
        uint8_t c;
        if (uart_read_bytes(LINK_UART, &c, 1, pdMS_TO_TICKS(20)) == 1) {
            if (c == '\n') {
                b[n] = 0;
                if (!strncmp(b, "S,", 2) || !strncmp(b, "HELLO", 5)) return true;
                n = 0;
            } else if (c != '\r' && n < (int)sizeof(b) - 1) {
                b[n++] = (char)c;
            }
        }
    }
    return false;
}

static void handle(const char *s)
{
    g_st.last_rx_ms = now_ms();
    g_st.rx_lines++;
    seb_cp_msg_t m;
    if (!seb_proto_parse_cp(s, &m)) {
        if (s[0]) ESP_LOGI(TAG, "от со-процессора: %s", s);
        return;
    }
    switch (m.type) {
    case CP_STATUS: {
        int old_mic = g_st.cp_mic;
        g_st.cp_t1 = m.t1; g_st.cp_t2 = m.t2; g_st.cp_t3 = m.t3;
        g_st.cp_mic = m.mic; g_st.cp_bat = m.bat_mv; g_st.cp_up = m.uptime;
        g_st.cp_bt = m.bt; g_st.cp_music = m.music; g_st.cp_mux = m.mux;
        if (m.mic && !old_mic) app_post(CMD_PTT, 0);         // нажали кнопку мика -> разговор
    } break;
    case CP_TOUCH:
        app_post(CMD_HW_TOUCH, m.n);
        break;
    case CP_RF_TRIG:
        ST_SET(rf_rx, "RF RX: ТРИГГЕР %d", m.n);
        g_st.rf_rx_ms = now_ms();
        ESP_LOGI(TAG, "RF: пойман триггер %d (код %lu)", m.n, (unsigned long)m.code);
        st_flash(COL_ORANGE, "RF ТРИГГЕР %d", m.n);
        break;
    case CP_RF_LIGHT:
        ST_SET(rf_rx, "RF RX: пульт #%d", m.n);
        g_st.rf_rx_ms = now_ms();
        break;
    case CP_RF_UNK:
        ST_SET(rf_rx, "RF RX: ?%lu", (unsigned long)m.code);
        g_st.rf_rx_ms = now_ms();
        break;
    case CP_RF_SENT:
        if (m.n == 22) ST_SET(rf_tx, "HOLD ok");
        else if (m.n >= 1 && m.n <= 21) ST_SET(rf_tx, "#%s ok", RF_NAMES[m.n - 1]);
        break;
    case CP_OTA:
        g_st.cp_ota = m.ota_state;
        g_st.cp_ota_pct = m.ota_pct;
        ST_SET(cp_ip, "%s", m.text);
        break;
    case CP_BAT:
        if (m.bat_level == 2) app_post(CMD_BAT_DOWN, m.bat_mv);
        break;
    case CP_GRACE:
        app_post(CMD_GRACE, m.bat_mv);
        break;
    case CP_FORCE_BTN:
        g_st.grace_forced = true;
        break;
    case CP_PONG:
        g_st.last_ack = (uint32_t)m.n;
        g_st.last_ack_ms = now_ms();
        break;
    case CP_HELLO:
        ESP_LOGI(TAG, "со-процессор стартовал (%s)", m.text);
        break;
    case CP_VERSION:
        ST_SET(cp_ver, "%s", m.text);
        ESP_LOGI(TAG, "версия со-процессора: %s", m.text);
        if (!strncmp(m.text, "c-idf", 5)) link_send_cfg();
        break;
    case CP_CFG_REQ:
        link_send_cfg();
        break;
    default:
        break;
    }
}

static void link_task(void *arg)
{
    static char lbuf[200];
    seb_linebuf_t lb;
    seb_linebuf_init(&lb, lbuf, sizeof(lbuf));
    uint8_t buf[128];
    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            g_st.rx_bytes++;
            if (seb_linebuf_put(&lb, (char)buf[i])) {
                if (lb.len && !lb.overflow) handle(lb.buf);
                seb_linebuf_reset(&lb);
            }
        }
    }
}

void link_start(void)
{
    link_uart_setup();
    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "UART1 к со-процессору: TX %d -> RX2 16, RX %d <- TX2 17, 115200", PIN_LINK_TX, PIN_LINK_RX);
}
