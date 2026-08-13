// Board/fs_hw_flash.c — see fs_hw_flash.h. Binds Robot/storage/fs to the
// RASC-generated g_flash0 (r_flash_lp) instance; Robot/ knows nothing
// about FSP or the flash_api_t shape.

#include <string.h>
#include "fs_hw_flash.h"
#include "fs.h"
#include "hal_data.h"
#include "printers.h"

// usart_drain isn't declared in cli_transport_usart.h — forward declare
// it locally, same as hal_entry.c does.
void usart_drain(void);

static void step(const char *msg) {
    print("nanofs: "); print(msg); printCr();
    usart_drain();
}

static bool hw_erase_sector(uint32_t sector) {
    uint32_t addr = BSP_FEATURE_FLASH_DATA_FLASH_START + sector * BSP_FEATURE_FLASH_LP_DF_BLOCK_SIZE;
    return g_flash0.p_api->erase(g_flash0.p_ctrl, addr, 1) == FSP_SUCCESS;
}

static bool hw_write(uint32_t offset, const uint8_t *data, uint32_t len) {
    uint32_t addr = BSP_FEATURE_FLASH_DATA_FLASH_START + offset;
    return g_flash0.p_api->write(g_flash0.p_ctrl, (uint32_t)data, addr, len) == FSP_SUCCESS;
}

static bool hw_read(uint32_t offset, uint8_t *data, uint32_t len) {
    // Data flash is memory-mapped for reads on the RA4M1 — no driver call
    // needed (flash_api_t has no read entry point at all).
    memcpy(data, (const void *)(BSP_FEATURE_FLASH_DATA_FLASH_START + offset), len);
    return true;
}

static const fs_hw_t g_fs_hw = {
    .sector_size = BSP_FEATURE_FLASH_LP_DF_BLOCK_SIZE,
    .sector_count = BSP_DATA_FLASH_SIZE_BYTES / BSP_FEATURE_FLASH_LP_DF_BLOCK_SIZE,
    .erase_sector = hw_erase_sector,
    .write = hw_write,
    .read = hw_read,
};

void fs_hw_flash_init(void) {
    step("opening g_flash0");
    if (g_flash0.p_api->open(g_flash0.p_ctrl, g_flash0.p_cfg) != FSP_SUCCESS) {
        step("g_flash0 open failed");
        return;
    }

    fs_bind_hw(&g_fs_hw);
    step("fs ready");
}
