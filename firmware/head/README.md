# head — голова (ESP32-S3-N16R8) на ESP-IDF v5.5.5

Как залить и проверить — в [`../README.md`](../README.md). Эталон поведения — Arduino v21 (`../../ESP32_Sebostian/ESP32 S3`).

| Файл `main/` | Что |
|---|---|
| `main.c` | порядок старта: сначала Wi-Fi + веб + OTA, потом остальное; безопасный режим |
| `board.h` | пины и калибровка тача |
| `settings.c` | NVS: Wi-Fi, пароль OTA, сервер, вывод, громкость, потолок, WAKE, AEC |
| `net.c` / `web.c` | Wi-Fi (+ точка доступа для настройки), NTP, mDNS / веб: `/status /setup /ota /log /screenshot /coredump /kws.tar /bat.tar` |
| `display.c` / `ui.c` | ST7789 + LVGL 9 + тач XPT2046 / 8 страниц и окна (OTA, «батарея села», «ВКЛЮЧИТЬ») |
| `audio.c` | I2S1 -> MAX98357A, микшер 48 кГц, ресемплер, дакинг, потолок + лимитер, мьюты, reference для AEC |
| `mic.c` | I2S0 <- INMP441, эхоподавление ESP-SR (AEC), шумодав, раздача звука |
| `voice.c` / `listener.c` | разговор с сервером (ASK/PTT/SSE/тесты/TRAIN) / слово «Себастьян» + перебивание |
| `mp3.c` `fx.c` `train.c` | MP3 с SD / стуки, «дзынь», тест мика / TRAIN |
| `link.c` `app.c` `power.c` `batlog.c` | UART к со-процессору / команды кнопок / защита батареи и сон / лог батареи |
| `fonts/` | DejaVu Sans с кириллицей (lv_font_conv, 14/16/24 px + цифры часов 56 px) |

`components/sr_lite` — минимум ESP-SR (AEC/NS), `components/dl_fft` — FFT для него. `sim/` — симулятор экрана на ПК.

Шрифты (если нужны новые символы — добавь диапазон и перегенерируй, `npm i lv_font_conv@1.5.2 dejavu-fonts-ttf@2.37.3`):
```
R="-r 0x20-0x7E -r 0x401,0x410-0x44F,0x451 -r 0xAB,0xB0,0xBB,0xD7,0x2013,0x2014,0x2026,0x2116,0x2190-0x2193,0x2212,0x25A0,0x25B6,0x25CF"
npx lv_font_conv --font DejaVuSansCondensed.ttf $R --size 14 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h -o seb_font_14.c
npx lv_font_conv --font DejaVuSansCondensed-Bold.ttf $R --size 16 ... -o seb_font_16.c   (и 24 -> seb_font_24.c)
npx lv_font_conv --font DejaVuSans-Bold.ttf -r 0x2D,0x30-0x3A --size 56 ... -o seb_font_clock.c
```
