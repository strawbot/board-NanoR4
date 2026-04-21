  /* generated memory regions file - HAND-EDITED for Arduino Nano R4 bootloader.
   *
   * The Arduino Nano R4 (VID 0x2341, PID 0x0374) ships with a DFU bootloader
   * that occupies the first 16 KB of code flash (0x0000–0x3FFF, reported by
   * `dfu-util -l` as "8*2Ka" — read-only to DFU). User code must live at
   * 0x4000 and the Cortex-M vector table must be at the start of that region
   * so VTOR lines up when the bootloader jumps to us.
   *
   * WARNING: RASC will overwrite this file if you re-run "Generate Project
   * Content". After every RASC regenerate, check this file — if FLASH_START
   * is back to 0x440 / FLASH_GAP_START is 0, restore this hand-edited copy
   * (git checkout Nano/memory_regions.ld), then `pio run -t clean && pio run`.
   *
   * Original RASC defaults (no-bootloader, raw-chip layout):
   *   FLASH_START = 0x00000440   FLASH_LENGTH = 0x0003FBC0
   *   FLASH_GAP_START = 0x0      FLASH_GAP_LENGTH = 0x400
   *   OPTION_SETTING_* at 0x400..0x437 (we now leave these alone — the
   *   bootloader programs them; emitting anything there would fail a DFU
   *   write against a read-only sector).
   */

  RAM_START = 0x20000000;
  RAM_LENGTH = 0x00008000;

  /* Move user-code region above the 16 KB bootloader. 120*2KB = 240 KB,
   * split into a 1 KB gap (vectors) and the remaining 239 KB (code/rodata). */
  FLASH_GAP_START  = 0x00004000;
  FLASH_GAP_LENGTH = 0x00000400;
  FLASH_START      = 0x00004400;
  FLASH_LENGTH     = 0x0003BC00;

  DATA_FLASH_START  = 0x40100000;
  DATA_FLASH_LENGTH = 0x00002000;

  /* Option settings live inside the bootloader region (read-only to us).
   * Zero-length them so fsp_gen.ld's OPTION_SETTING output sections emit
   * nothing. If a future build complains about overflow, it means some code
   * added a `.option_setting_ofs*` section and we'll need to strip or redirect
   * it — the bootloader already configured OFS0/OFS1/SECMPU. */
  OPTION_SETTING_OFS0_START    = 0x00000400;
  OPTION_SETTING_OFS0_LENGTH   = 0x00000000;
  OPTION_SETTING_OFS1_START    = 0x00000404;
  OPTION_SETTING_OFS1_LENGTH   = 0x00000000;
  OPTION_SETTING_SECMPU_START  = 0x00000408;
  OPTION_SETTING_SECMPU_LENGTH = 0x00000000;
  OPTION_SETTING_OSIS_START    = 0x01010018;
  OPTION_SETTING_OSIS_LENGTH   = 0x00000000;
