/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes   <System Includes> , "Project Includes"
 **********************************************************************************************************************/
#include "bsp_api.h"

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

#if (BSP_FEATURE_ICU_NMIER_MAX_INDEX > 15U)
 #define BSP_PRV_NMIER_T    uint32_t
#else
 #define BSP_PRV_NMIER_T    uint16_t
#endif

/***********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Exported global variables (to be accessed by other files)
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private global variables and functions
 **********************************************************************************************************************/

/** This array holds callback functions. */
bsp_grp_irq_cb_t g_bsp_group_irq_sources[BSP_FEATURE_ICU_NMIER_MAX_INDEX + 1] BSP_SECTION_EARLY_INIT;

void        NMI_Handler(void);
static void bsp_group_irq_call(bsp_grp_irq_t irq);

/***********************************************************************************************************************
 * Board patch (not upstream FSP): an NMI source with no registered callback
 * used to be silently dropped -- fine for a source that's merely unexpected,
 * but fatal for one whose underlying condition can't resolve itself before
 * the handler returns. Observed in practice: BSP_GRP_IRQ_MPU_STACK firing
 * right after a debug-probe-driven reset/reflash (never after a normal
 * power-on boot) -- if the stack pointer is genuinely out of the MPU's
 * guarded range, even the exception-return epilogue needs to touch that
 * same bad stack, so NMI_Handler re-faults immediately on return and the
 * CPU is stuck until something external (a debugger-issued reset) breaks
 * the cycle. See board-NanoR4/Docs/usb-handoff.md.
 *
 * Fix: stash which source fired, MSP/PSP, and the MPU's own config
 * (survives the reset, VBATT backup registers -- board-NanoR4/Board/fault.c
 * uses byte offsets 4-27 for its own fault marker, board-NanoR4/Board/
 * usb_init.c's ISR snapshot uses 32-52; this uses 56+), then force a full
 * system reset via NVIC_SystemReset() instead of attempting a normal
 * return. That call needs only a small amount of the CURRENT stack for its
 * own local state -- it never performs an exception-return epilogue, so it
 * can escape even when the stack that epilogue would need is the thing
 * that's broken. Capturing MSP/PSP/MPU state (rather than just resetting
 * blind) is what actually confirms or refutes "stack pointer genuinely
 * out of the guarded range" instead of just working around it.
 *
 * Read back after reboot the same way as board-NanoR4/Board/fault.c's own
 * marker -- `hex 4001E538 22 dump` (offset 56 = 0x4001E500 + 56):
 *     0x4001E538  marker 0xBAD00101 - present only if this path fired
 *     0x4001E53C  irq (bsp_grp_irq_t)     -- 12 = BSP_GRP_IRQ_MPU_STACK
 *     0x4001E540  MSP
 *     0x4001E544  PSP
 *     0x4001E548  MPU->CTRL
 *     0x4001E54C  MPU->RNR (as last left, before the dump loop below runs)
 *     0x4001E550..  8 regions x (RBAR, RASR), 8 bytes each
 **********************************************************************************************************************/
#define BSP_PRV_NMI_UNHANDLED_MARKER  0xBAD00101UL
#define BSP_PRV_PRCR_PRC1_UNLOCK      0xA502U
#define BSP_PRV_PRCR_LOCK             0xA500U
#define BSP_PRV_VBTBER_VBAE           (1U << 3)
#define BSP_PRV_VBT_WORD(off)         (*(volatile uint32_t *) &R_SYSTEM->VBTBKR[(off)])
#define BSP_PRV_MPU_REGION_COUNT      8U   // Cortex-M4 ARMv7-M MPU, this part

static void bsp_group_irq_unhandled_reset (bsp_grp_irq_t irq)
{
    uint32_t msp = __get_MSP();
    uint32_t psp = __get_PSP();

    R_SYSTEM->PRCR   = BSP_PRV_PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = BSP_PRV_VBTBER_VBAE;

    BSP_PRV_VBT_WORD(56) = BSP_PRV_NMI_UNHANDLED_MARKER;
    BSP_PRV_VBT_WORD(60) = (uint32_t) irq;
    BSP_PRV_VBT_WORD(64) = msp;
    BSP_PRV_VBT_WORD(68) = psp;
    BSP_PRV_VBT_WORD(72) = MPU->CTRL;
    BSP_PRV_VBT_WORD(76) = MPU->RNR;

    for (uint32_t region = 0; region < BSP_PRV_MPU_REGION_COUNT; region++)
    {
        MPU->RNR = region;
        BSP_PRV_VBT_WORD(80 + (region * 8U))     = MPU->RBAR;
        BSP_PRV_VBT_WORD(80 + (region * 8U) + 4U) = MPU->RASR;
    }

    R_SYSTEM->PRCR = BSP_PRV_PRCR_LOCK;

    // Auto-resetting is only safe when nothing else is in control. With a
    // debugger attached, NVIC_SystemReset() yanks the target out from under
    // an active GDB/JLinkGDBServer session -- observed in practice: VS
    // Code's Stop button stops responding and the debug UI is left stuck
    // even after the backend process has already exited, needing a manual
    // "Debug: Stop" / window reload to clear. Same DHCSR.C_DEBUGEN check
    // TinyUSB's own TU_BREAKPOINT() uses elsewhere in this codebase. When a
    // debugger IS attached, park here instead -- it can halt/reset via SWD
    // regardless of what the CPU is doing, same as the manual
    // debugger-restart workaround that already reliably recovers this.
    if (0U == (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk))
    {
        NVIC_SystemReset();
    }

    for (;;)
    {
        __NOP();
    }
}

/*******************************************************************************************************************//**
 * Calls the callback function for an interrupt if a callback has been registered.
 *
 * @param[in]   irq         Which interrupt to check and possibly call.
 *
 * @retval FSP_SUCCESS              Callback was called.
 * @retval FSP_ERR_INVALID_ARGUMENT No valid callback has been registered for this interrupt source.
 *
 * @warning This function is called from within an interrupt
 **********************************************************************************************************************/
static void bsp_group_irq_call (bsp_grp_irq_t irq)
{
    /** Check for valid callback */
    if (NULL != g_bsp_group_irq_sources[irq])
    {
        /** Callback has been found. Call it. */
        g_bsp_group_irq_sources[irq](irq);
    }
    else
    {
        /** No callback registered -- see the board patch comment above
         *  bsp_group_irq_unhandled_reset(). Silently doing nothing here is
         *  what let an unresolvable condition (e.g. BSP_GRP_IRQ_MPU_STACK)
         *  re-fault forever instead of ever making forward progress. */
        bsp_group_irq_unhandled_reset(irq);
    }
}

/*******************************************************************************************************************//**
 * @addtogroup BSP_MCU
 *
 * @{
 **********************************************************************************************************************/

/*******************************************************************************************************************//**
 * Register a callback function for supported interrupts. If NULL is passed for the callback argument then any
 * previously registered callbacks are unregistered.
 *
 * @param[in]  irq          Interrupt for which  to register a callback.
 * @param[in]  p_callback   Pointer to function to call when interrupt occurs.
 *
 * @retval     FSP_SUCCESS                  Callback registered
 * @retval     FSP_ERR_ASSERTION            Callback pointer is NULL
 **********************************************************************************************************************/
fsp_err_t R_BSP_GroupIrqWrite (bsp_grp_irq_t irq, void (* p_callback)(bsp_grp_irq_t irq))
{
#if BSP_CFG_PARAM_CHECKING_ENABLE

    /* Check pointer for NULL value. */
    FSP_ASSERT(p_callback);
#endif

    /* Register callback. */
    g_bsp_group_irq_sources[irq] = p_callback;

    return FSP_SUCCESS;
}

/*******************************************************************************************************************//**
 * Non-maskable interrupt handler. This exception is defined by the BSP, unlike other system exceptions, because
 * there are many sources that map to the NMI exception.
 **********************************************************************************************************************/
void NMI_Handler (void)
{
    /* NMISR is masked by NMIER to prevent iterating over NMI status flags that are not enabled. */
    BSP_PRV_NMIER_T nmier = R_ICU->NMIER;
    BSP_PRV_NMIER_T nmisr = R_ICU->NMISR & nmier;

    /* Loop over all NMI status flags */
    for (bsp_grp_irq_t irq = BSP_GRP_IRQ_IWDT_ERROR; irq <= (bsp_grp_irq_t) (BSP_FEATURE_ICU_NMIER_MAX_INDEX); irq++)
    {
        /* If the current irq status register is set call the irq callback. */
        if (0U != (nmisr & (1U << irq)))
        {
            (void) bsp_group_irq_call(irq);
        }
    }

    /* Clear status flags that have been handled. */
    R_ICU->NMICLR = nmisr;

#if BSP_CFG_MCU_PART_SERIES == 8

    /* Wait for NMISR to be cleared before exiting the ISR to prevent the IRQ from being regenerated.
     * See "NMICLR : Non-Maskable Interrupt Status Clear Register" description in the ICU section
     * of the relevant hardware manual */
    FSP_HARDWARE_REGISTER_WAIT((R_ICU->NMISR & nmisr), 0);
#endif
}

/** @} (end addtogroup BSP_MCU) */
