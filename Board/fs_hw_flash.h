// Board/fs_hw_flash.h — Nano's hardware bind for Robot/storage/fs.

#ifndef BOARD_FS_HW_FLASH_H
#define BOARD_FS_HW_FLASH_H

// Opens the RASC-generated g_flash0 (r_flash_lp) instance and binds
// Robot/storage/fs to it (mounting, formatting once if the volume is
// blank or unreadable). Call once, after init_tea() (no scheduler
// dependency, but keeps ordering consistent with the other hardware
// bring-up calls in hal_entry.c).
void fs_hw_flash_init(void);

#endif // BOARD_FS_HW_FLASH_H
