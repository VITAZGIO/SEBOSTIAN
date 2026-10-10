// Пины головы (ESP32-S3-N16R8). Проверено на стенде (Arduino v21), см. ТЗ §2.2.
// НЕЛЬЗЯ: 35-37 (octal PSRAM), 0/45/46 (strapping), 19/20 (USB), 26-32 (флеш), 43/44 (монитор).
#pragma once

// Дисплей ST7789 240x320 + тач XPT2046 — общая шина SPI2
#define PIN_LCD_SCK     12
#define PIN_LCD_MOSI    11
#define PIN_LCD_MISO    13
#define PIN_LCD_CS      10
#define PIN_LCD_DC      9
#define PIN_LCD_RST     14
#define PIN_LCD_BL      21
#define PIN_TOUCH_CS    16
#define PIN_TOUCH_IRQ   17      // LOW = касание

// SD-карта — своя шина SPI3, 10 МГц, только FAT32
#define PIN_SD_SCK      6
#define PIN_SD_MOSI     7
#define PIN_SD_MISO     15
#define PIN_SD_CS       5

// I2S1 -> 2x MAX98357A (динамики)
#define PIN_SPK_BCLK    40
#define PIN_SPK_LRC     41
#define PIN_SPK_DIN     42
#define PIN_SPK_MUTE    2       // базы 2x BC547: LOW = играют, HIGH = молчат
#define PIN_XSMT        39      // PCM5102A XSMT (звук со-процессора в AUX): HIGH = играет
#define PIN_AUX_DET     3       // детект штекера AUX: HIGH = вставлен

// I2S0 <- микрофон INMP441 (L/R на GND = левый канал)
#define PIN_MIC_SCK     47
#define PIN_MIC_WS      4       // было 48: там бортовой RGB, такт мика его зажигал
#define PIN_MIC_SD      38      // на схеме 37 — он занят PSRAM, реально 38

// UART1 <-> со-процессор (ESP32 UART2)
#define PIN_LINK_TX     8       // -> ESP32 RX2 (16)
#define PIN_LINK_RX     18      // <- ESP32 TX2 (17)
#define LINK_UART       UART_NUM_1

#define PIN_RGB_LED     48      // бортовой WS2812 — гасим при старте

// Калибровка тача (сырые XPT2046, сняты в Arduino при rotation 0, TFT_eSPI {460, 3320, 340, 3430, 1}):
// экранный x берётся из канала Y (команда 0x90), экранный y — из канала X (0xD0), потом зеркалим (экран на 180°).
#define TOUCH_X_MIN     460
#define TOUCH_X_RANGE   3320
#define TOUCH_Y_MIN     340
#define TOUCH_Y_RANGE   3430
#define TOUCH_Z_MIN     600     // порог нажатия (как getTouch в TFT_eSPI)

#define LCD_W           240
#define LCD_H           320
