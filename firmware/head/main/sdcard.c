#include "sdcard.h"
#include <sys/stat.h>
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

static const char *TAG = "sd";
static bool s_ok;
static uint32_t s_mb;

bool sdcard_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 10000;
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI3_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mc, &card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD не смонтировалась (%s) — карта FAT32? провода 6/7/15/5?", esp_err_to_name(e));
        return false;
    }
    s_mb = (uint32_t)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    s_ok = true;
    ESP_LOGI(TAG, "SD OK, %lu МБ", (unsigned long)s_mb);
    return true;
}

bool sdcard_ok(void) { return s_ok; }
uint32_t sdcard_size_mb(void) { return s_mb; }

bool sd_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void sd_mkdir(const char *path)
{
    if (!sd_exists(path)) mkdir(path, 0775);
}
