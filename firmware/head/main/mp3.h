// MP3 с SD (/sdcard/test.mp3), декодер libhelix. Играет по кругу после PLAY, без автостарта.
#pragma once
#include <stdbool.h>

#define MP3_FILE "/sdcard/test.mp3"

void mp3_init(void);
bool mp3_play(void);        // false — нет SD/файла
void mp3_stop(void);
bool mp3_playing(void);
void mp3_toggle(void);
