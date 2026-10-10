// SD-карта на своей шине SPI3 (10 МГц), FAT32, монтируется в /sdcard.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SD_MOUNT "/sdcard"

bool sdcard_init(void);
bool sdcard_ok(void);
uint32_t sdcard_size_mb(void);
bool sd_exists(const char *path);
void sd_mkdir(const char *path);
