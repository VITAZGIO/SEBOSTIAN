#include "display.h"
#include <string.h>
#include <stdlib.h>
#include "board.h"
#include "state.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "disp";
#define LCD_HOST       SPI2_HOST
#define LCD_PCLK_HZ    (26 * 1000 * 1000)     // в Arduino было 27 МГц — работало
#define DRAW_LINES     40

static esp_lcd_panel_handle_t s_panel;
static spi_device_handle_t s_touch;
static lv_display_t *s_disp;
static SemaphoreHandle_t s_lock;
static volatile int64_t s_touch_ms;
static volatile bool s_touch_down;

bool disp_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void disp_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

int64_t touch_last_ms(void) { return s_touch_ms; }
bool touch_down_now(void) { return gpio_get_level(PIN_TOUCH_IRQ) == 0; }

void display_backlight(bool on) { gpio_set_level(PIN_LCD_BL, on ? 1 : 0); }

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    lv_draw_sw_rgb565_swap(px, (uint32_t)lv_area_get_size(a));    // SPI шлёт старший байт первым
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------- тач XPT2046 ----------
static uint16_t xpt_read(uint8_t cmd)
{
    spi_transaction_t t = {0};
    t.length = 24;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = cmd;
    if (spi_device_polling_transmit(s_touch, &t) != ESP_OK) return 0;
    return (uint16_t)(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0x0FFF;
}

// Как TFT_eSPI::getTouch: давление, два близких замера подряд, калибровка, зеркало (экран на 180°)
static bool touch_get(int32_t *x, int32_t *y)
{
    int z1 = xpt_read(0xB0), z2 = xpt_read(0xC0);
    int z = 4095 + z1 - z2;
    if (z == 4095) z = 0;
    if (z < TOUCH_Z_MIN) return false;
    uint16_t rx1 = 0, ry1 = 0, rx2, ry2;
    for (int i = 0; i < 3; i++) { rx1 = xpt_read(0xD0); ry1 = xpt_read(0x90); }
    rx2 = xpt_read(0xD0);
    ry2 = xpt_read(0x90);
    if (abs((int)rx1 - (int)rx2) > 20 || abs((int)ry1 - (int)ry2) > 20) return false;
    int rawx = (rx1 + rx2) / 2, rawy = (ry1 + ry2) / 2;
    int xx = (rawy - TOUCH_X_MIN) * LCD_W / TOUCH_X_RANGE;
    int yy = (rawx - TOUCH_Y_MIN) * LCD_H / TOUCH_Y_RANGE;
    xx = LCD_W - 1 - xx;
    yy = LCD_H - 1 - yy;
    if (xx < 0) xx = 0;
    if (xx >= LCD_W) xx = LCD_W - 1;
    if (yy < 0) yy = 0;
    if (yy >= LCD_H) yy = LCD_H - 1;
    *x = xx;
    *y = yy;
    return true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t lx, ly;
    data->state = LV_INDEV_STATE_RELEASED;
    if (gpio_get_level(PIN_TOUCH_IRQ) == 0) {           // тач читаем только по IRQ
        int32_t x, y;
        if (touch_get(&x, &y)) {
            lx = x;
            ly = y;
            data->state = LV_INDEV_STATE_PRESSED;
            s_touch_ms = esp_timer_get_time() / 1000;
            if (!s_touch_down) ESP_LOGD(TAG, "касание %ld,%ld", (long)x, (long)y);
            s_touch_down = true;
        }
    }
    if (data->state == LV_INDEV_STATE_RELEASED) s_touch_down = false;
    data->point.x = lx;
    data->point.y = ly;
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t ms = 10;
        if (disp_lock(100)) {
            ms = lv_timer_handler();
            disp_unlock();
        }
        if (ms < 5) ms = 5;
        if (ms > 30) ms = 30;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

esp_err_t display_init(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;
    s_lock = xSemaphoreCreateRecursiveMutex();
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    display_backlight(false);
    gpio_config_t irq = {.pin_bit_mask = 1ULL << PIN_TOUCH_IRQ, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&irq);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * DRAW_LINES * 2 + 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    s_disp = NULL;
    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(LCD_W, LCD_H);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx = s_disp,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io), TAG, "io");
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,     // проверено: BGR
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel), TAG, "st7789");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, false);          // проверено: инверсия OFF
    esp_lcd_panel_mirror(s_panel, true, true);           // экран стоит перевёрнутым (180°)
    esp_lcd_panel_disp_on_off(s_panel, true);

    // тач на той же шине: свой CS и своя частота 2 МГц
    spi_device_interface_config_t tcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &tcfg, &s_touch), TAG, "touch");

    size_t bsz = LCD_W * DRAW_LINES * 2;
    void *b1 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, b1, b2, bsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    lv_indev_t *ind = lv_indev_create();
    lv_indev_set_type(ind, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ind, touch_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "дисплей ST7789 240x320 + тач XPT2046 готовы");
    return ESP_OK;
}

// ---------- снимок экрана (BMP 16 бит 565) ----------
uint8_t *display_snapshot_bmp(size_t *len)
{
    *len = 0;
    if (!disp_lock(1000)) return NULL;
    lv_draw_buf_t *db = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    disp_unlock();
    if (!db) return NULL;
    const int w = db->header.w, h = db->header.h, stride = db->header.stride;
    const size_t row = (size_t)w * 2, hdr = 14 + 40 + 12;
    size_t total = hdr + row * h;
    uint8_t *b = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (b) {
        memset(b, 0, hdr);
        b[0] = 'B'; b[1] = 'M';
        memcpy(b + 2, &total, 4);
        uint32_t off = hdr, dib = 40, comp = 3, isz = row * h;
        int32_t bw = w, bh = -h;           // сверху вниз
        uint16_t planes = 1, bpp = 16;
        memcpy(b + 10, &off, 4);
        memcpy(b + 14, &dib, 4);
        memcpy(b + 18, &bw, 4);
        memcpy(b + 22, &bh, 4);
        memcpy(b + 26, &planes, 2);
        memcpy(b + 28, &bpp, 2);
        memcpy(b + 30, &comp, 4);
        memcpy(b + 34, &isz, 4);
        uint32_t masks[3] = {0xF800, 0x07E0, 0x001F};
        memcpy(b + 54, masks, 12);
        for (int y = 0; y < h; y++) memcpy(b + hdr + y * row, db->data + y * stride, row);
        *len = total;
    }
    lv_draw_buf_destroy(db);
    return b;
}
