/*
 *  Linker File for Renesas FSP — Arduino Nano R4 (bootloader in first 16 KB)
 *
 *  The Nano R4 bootloader has already programmed OFS0, OFS1, SECMPU, and OSIS
 *  (they live in the read-only 0x0000–0x3FFF region). We must NOT emit content
 *  for them or the link fails against zero-length OPTION_SETTING_* regions
 *  (see memory_regions_custom.ld).
 *
 *  The /DISCARD/ block below consumes the input sections before fsp_gen.ld's
 *  KEEP(...) clauses try to place them. Order matters: this SECTIONS runs
 *  before fsp_gen.ld's — first match wins.
 */

INCLUDE memory_regions_custom.ld

SECTIONS
{
    /DISCARD/ :
    {
        *(.option_setting_ofs0)
        *(.option_setting_ofs1)
        *(.option_setting_secmpu)
        *(.option_setting_osis)
    }
}

INCLUDE fsp_gen.ld
