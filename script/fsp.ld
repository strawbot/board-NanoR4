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

/* Canary linker symbols — Robot/diagnostics/canary/canary.h contract.
 *
 * On Renesas RA4M1 the main stack lives in the noinit section as g_main_stack
 * (section .bss.g_main_stack), placed immediately before any .ram_noinit/.noinit
 * content.  The Cortex-M SP is initialised to __ram_noinit$$Limit, which is
 * therefore a correct _estack.  _sstack is set _Min_Stack_Size bytes below that,
 * which must be <= BSP_CFG_STACK_MAIN_BYTES (configured in bsp_cfg.h).
 *
 * These symbols are defined here (after INCLUDE fsp_gen.ld) so that
 * __ram_noinit$$Limit is already resolved by the time they are evaluated.
 * See Robot/README.md for the full adoption checklist. */
_estack        = __ram_noinit$$Limit;
_Min_Stack_Size = 0x800;              /* 2 KB — must be <= BSP_CFG_STACK_MAIN_BYTES */
_sstack        = _estack - _Min_Stack_Size;
