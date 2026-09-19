/*
 * microSD slot on the LCDwiki / Hosyond ES3C35P, wired for 4-bit SD mode on the
 * ESP32-S3's SDMMC host. There is no card-detect line, so a card is only seen at boot.
 */
#include "bsp_sdcard.h"

#include "bsp_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static uint64_t s_total;
static uint64_t s_free;

esp_err_t bsp_sdcard_init(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BSP_SD_CLK;
    slot.cmd = BSP_SD_CMD;
    slot.d0 = BSP_SD_D0;
    slot.d1 = BSP_SD_D1;
    slot.d2 = BSP_SD_D2;
    slot.d3 = BSP_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "no card mounted: %s", err == ESP_FAIL ? "unreadable, or no FAT filesystem" : esp_err_to_name(err));
        return err;
    }

    // Computing free space can scan the whole FAT on a large card, so do it once here
    esp_vfs_fat_info(BSP_SDCARD_MOUNT_POINT, &s_total, &s_free);
    ESP_LOGI(TAG, "mounted \"%s\" at %s: %.1f GB, %.1f GB free, %d MHz, %d-bit", s_card->cid.name,
             BSP_SDCARD_MOUNT_POINT, s_total / 1e9, s_free / 1e9, s_card->real_freq_khz / 1000,
             s_card->log_bus_width ? 1 << s_card->log_bus_width : 1);
    return ESP_OK;
}

bool bsp_sdcard_get_space(uint64_t *total, uint64_t *free)
{
    if (!s_card) {
        return false;
    }
    *total = s_total;
    *free = s_free;
    return true;
}
