#include "mp3.h"
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "sdcard.h"
#include "state.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "mp3";
#define INBUF 8192

static volatile bool s_want, s_playing;
static TaskHandle_t s_task;

bool mp3_playing(void) { return s_playing; }

static size_t skip_id3(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
        size_t sz = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        fseek(f, (long)(10 + sz), SEEK_SET);
        return 10 + sz;
    }
    fseek(f, 0, SEEK_SET);
    return 0;
}

static void play_file(HMP3Decoder dec, uint8_t *in, int16_t *pcm)
{
    FILE *f = fopen(MP3_FILE, "rb");
    if (!f) { ESP_LOGE(TAG, "файл %s не открылся", MP3_FILE); s_want = false; return; }
    skip_id3(f);
    int left = 0;
    uint8_t *p = in;
    int fmt_rate = 0, fmt_ch = 0;
    seb_ring_t *r = audio_mp3_ring();
    bool eof = false;
    while (s_want) {
        if (left < 2048 && !eof) {                      // подлить данных из файла
            memmove(in, p, left);
            p = in;
            int n = fread(in + left, 1, INBUF - left, f);
            if (n <= 0) eof = true;
            else left += n;
        }
        if (left <= 0) break;
        int off = MP3FindSyncWord(p, left);
        if (off < 0) { left = 0; if (eof) break; continue; }
        p += off;
        left -= off;
        int err = MP3Decode(dec, &p, &left, pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof) break;
            continue;
        }
        if (err) { if (left > 0) { p++; left--; } continue; }   // битый кадр — ищем следующий
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.samprate != fmt_rate || fi.nChans != fmt_ch) {
            // новая частота/каналы: дождаться, пока старое доиграет, и сменить формат
            while (s_want && seb_ring_fill(r) > 0) vTaskDelay(pdMS_TO_TICKS(5));
            fmt_rate = fi.samprate;
            fmt_ch = fi.nChans;
            audio_mp3_format(fmt_rate, fmt_ch);
            ESP_LOGI(TAG, "%d Гц, %d кан., %d кбит/с", fmt_rate, fmt_ch, fi.bitrate / 1000);
        }
        uint32_t n = (uint32_t)fi.outputSamps, done = 0;
        while (s_want && done < n) {
            done += seb_ring_write(r, pcm + done, n - done);
            if (done < n) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    fclose(f);
}

static void mp3_task(void *arg)
{
    uint8_t *in = heap_caps_malloc(INBUF, MALLOC_CAP_INTERNAL);
    int16_t *pcm = heap_caps_malloc(1152 * 2 * 2, MALLOC_CAP_INTERNAL);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_want) {
            HMP3Decoder dec = MP3InitDecoder();
            if (!dec) { ESP_LOGE(TAG, "нет памяти под декодер"); s_want = false; break; }
            s_playing = true;
            ESP_LOGI(TAG, "старт %s", MP3_FILE);
            play_file(dec, in, pcm);
            MP3FreeDecoder(dec);
            if (s_want) ESP_LOGI(TAG, "трек закончился — по кругу");
        }
        audio_mp3_flush();
        s_playing = false;
        ESP_LOGI(TAG, "стоп");
    }
}

void mp3_init(void)
{
    xTaskCreatePinnedToCore(mp3_task, "mp3", 6144, NULL, 12, &s_task, 0);
}

bool mp3_play(void)
{
    if (!sdcard_ok()) { st_flash(COL_RED, "Нет SD-карты"); return false; }
    if (!sd_exists(MP3_FILE)) { st_flash(COL_RED, "Нет файла test.mp3"); return false; }
    if (s_playing) return true;
    s_want = true;
    s_playing = true;
    xTaskNotifyGive(s_task);
    return true;
}

void mp3_stop(void)
{
    s_want = false;
    for (int i = 0; i < 100 && s_playing; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void mp3_toggle(void)
{
    if (s_playing) mp3_stop();
    else mp3_play();
}
