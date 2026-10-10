#include "app.h"
#include <string.h>
#include "board.h"
#include "state.h"
#include "settings.h"
#include "audio.h"
#include "mp3.h"
#include "link.h"
#include "voice.h"
#include "listener.h"
#include "train.h"
#include "batlog.h"
#include "power.h"
#include "sysmon.h"
#include "net.h"
#include "seb_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "app";

// Подписи кнопок пульта на странице RADIO: до ~5 символов. Номер лучше оставить.
const char *RF_NAMES[21] = {
    "1 ON", "2", "3", "4", "5", "6", "7",
    "8", "9", "10", "11", "12", "13", "14",
    "15", "16", "17", "18", "19", "20", "21"};

typedef struct {
    app_cmd_t c;
    int arg;
} msg_t;

static QueueHandle_t s_q;

void app_post(app_cmd_t c, int arg)
{
    msg_t m = {c, arg};
    if (s_q) xQueueSend(s_q, &m, 0);
}

static void post_job(job_type_t t)
{
    job_t j = {.type = t};
    if (voice_busy() && t != JOB_KDEL) { st_flash(COL_YELLOW, "Занят — подожди"); return; }
    if (!voice_post(&j)) st_flash(COL_YELLOW, "Занят — подожди");
}

static void handle(const msg_t *m)
{
    switch (m->c) {
    case CMD_NONE:
        if (m->arg == 2) train_scan();
        break;
    case CMD_PLAY: mp3_toggle(); break;
    case CMD_VOLM: audio_volume_step(-1); break;
    case CMD_VOLP: audio_volume_step(+1); break;
    case CMD_SPK_MUTE: g_st.spk_muted = !g_st.spk_muted; audio_apply_outputs(); break;
    case CMD_AUX_MUTE: g_st.aux_muted = !g_st.aux_muted; audio_apply_outputs(); break;
    case CMD_AUX_MUSIC: link_send("MUSIC"); break;
    case CMD_LED: link_send("LED"); break;
    case CMD_BEEP: link_send("BEEP"); break;
    case CMD_LR_TEST: post_job(JOB_LR_TEST); break;
    case CMD_MIC_TEST: post_job(JOB_MIC_TEST); break;
    case CMD_ASK: post_job(JOB_ASK); break;
    case CMD_PING: post_job(JOB_PING); break;
    case CMD_KPOS: post_job(JOB_KPOS); break;
    case CMD_KNEG: post_job(JOB_KNEG); break;
    case CMD_KBG: post_job(JOB_KBG); break;
    case CMD_KPLAY: post_job(JOB_KPLAY); break;
    case CMD_KDEL: post_job(JOB_KDEL); break;
    case CMD_KTIP: train_next_tip(); break;
    case CMD_CPOTA:
        if (!strncmp(g_st.cp_ver, "c-idf", 5)) link_send_cfg();   // IDF-со-процессору нужен Wi-Fi (Arduino не нужен)
        link_send("OTA");
        g_st.cp_ota = 1;
        ST_SET(cp_ip, " ");
        ESP_LOGI(TAG, "-> со-процессор: режим прошивки");
        break;
    case CMD_REBOOT:
        st_flash(COL_RED, "ПЕРЕЗАГРУЗКА...");
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
        break;
    case CMD_WAKE:
        g_set.wake_on = !g_set.wake_on;
        settings_save();
        if (g_set.wake_on) listener_start();
        break;
    case CMD_AEC:
        if (m->arg) g_set.ns_on = !g_set.ns_on;
        else g_set.aec_on = !g_set.aec_on;
        settings_save();
        break;
    case CMD_OUT:
        if (m->arg >= OUT_SPK && m->arg <= OUT_BOTH && g_set.out_mode != m->arg) {
            g_set.out_mode = m->arg;
            settings_save();
            audio_apply_outputs();
        }
        break;
    case CMD_RF:
        if (m->arg >= 1 && m->arg <= 21) {
            link_send("RF %d", m->arg);
            ST_SET(rf_tx, "#%s ...", RF_NAMES[m->arg - 1]);
        } else if (m->arg == 22) {
            link_send("RFHOLD");
            ST_SET(rf_tx, "HOLD ...");
        }
        break;
    case CMD_HW_TOUCH:
        if (voice_busy()) break;            // во время разговора сенсоры не трогают музыку
        ESP_LOGI(TAG, "сенсор %d -> %s", m->arg, m->arg == 1 ? "тише" : m->arg == 2 ? "play/stop" : "громче");
        if (m->arg == 1) audio_volume_step(-1);
        if (m->arg == 2) mp3_toggle();
        if (m->arg == 3) audio_volume_step(+1);
        break;
    case CMD_PTT:
        if (!voice_busy()) { job_t j = {.type = JOB_PTT}; voice_post(&j); }
        break;
    case CMD_GRACE: power_grace(m->arg); break;
    case CMD_BAT_DOWN: power_bat_shutdown(m->arg); break;
    }
}

static void app_task(void *arg)
{
    int64_t t_sec = 0, t_log = 0, aux_change = 0;
    bool aux_raw = g_st.aux_plug;
    for (;;) {
        msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(50)) == pdTRUE) handle(&m);
        int64_t now = now_ms();
        // детект штекера AUX, 150 мс антидребезга
        bool r = gpio_get_level(PIN_AUX_DET) == 1;
        if (r != aux_raw) { aux_raw = r; aux_change = now; }
        else if (r != g_st.aux_plug && now - aux_change > 150) {
            g_st.aux_plug = r;
            ESP_LOGI(TAG, "штекер AUX %s", r ? "ВСТАВЛЕН" : "вынут");
            audio_apply_outputs();
        }
        listener_tick();
        if (now - t_sec >= 1000) {
            t_sec = now;
            link_send("PING %lu", (unsigned long)++g_st.ping_sent);
            batlog_tick();
            power_tick();
            sysmon_tick();
        }
        if (now - t_log >= 5000) {
            t_log = now;
            char ip[32];
            net_ip_str(ip, sizeof(ip));
            ESP_LOGI(TAG, "mp3:%s vol:%.2f | связь %s/%s | акк:%dмВ | Wi-Fi:%s | RAM %u PSRAM %u",
                     mp3_playing() ? "play" : "stop", g_set.volume, link_rx_ok() ? "OK" : "НЕТ", link_tx_ok() ? "OK" : "НЕТ",
                     g_st.cp_bat, ip, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

void app_start(void)
{
    s_q = xQueueCreate(16, sizeof(msg_t));
    train_scan();
    xTaskCreatePinnedToCore(app_task, "app", 6144, NULL, 6, NULL, 0);
}
