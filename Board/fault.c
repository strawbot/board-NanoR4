// fault.c — make a crash distinguishable from a hang.
//
// FSP's default exception handlers are silent `while (1)` loops (startup.c:64),
// so a HardFault is indistinguishable from a stall: the board stops, nothing on
// the console, no LED. These need very different fixes, so it is worth being
// able to tell them apart.
//
// This overrides the weak fault handlers and:
//
//   1. Stashes the stacked PC/LR/xPSR plus CFSR/HFSR into the VBATT backup
//      registers, which survive a reset. Read them out from the CLI after the
//      board comes back up:
//
//          hex
//          4001E504 2 dump
//
//      Layout (all 32-bit):
//          0x4001E504  marker 0xFA017ED0 - present only if we actually faulted
//          0x4001E508  PC    - the faulting instruction
//          0x4001E50C  LR
//          0x4001E510  xPSR
//          0x4001E514  CFSR  - Configurable Fault Status  (SCB 0xE000ED28)
//          0x4001E518  HFSR  - HardFault Status           (SCB 0xE000ED2C)
//
//      VBTBKR[0..3] is the bootloader's double-tap magic - do not use it.
//      This starts at byte offset 4, which the bootloader never touches.
//
//   2. Blinks 20 fast pulses, pauses, repeats for 3 cycles — so a crash
//      announces itself even with no console attached — then resets the
//      board so it can resume normal operation rather than hang forever.
//
// Setting VBTBER.VBAE first is what makes the stash survive; without it the
// backup registers accept writes and read back correctly but retain nothing.
// The stashed fault info survives the reset that follows, so it can still be
// read out from the CLI after the board comes back up.
//
// Resolve a captured PC with:
//     arm-none-eabi-addr2line -f -e .pio/build/nano_r4/firmware.elf <PC>

#include <stdint.h>

#include "hal_data.h"
#include "sr595.h"
#include "muscle_sr.h"

void blink(void);                 // hal_entry.c - GPIO + busy loop, no deps

void fault_report(uint32_t *frame);   // called from the naked stubs below

#define FAULT_MARKER      0xFA017ED0UL
#define PRCR_PRC1_UNLOCK  0xA502U
#define PRCR_LOCK         0xA500U
#define VBTBER_VBAE       (1U << 3)
#define FAULT_BLINK_CYCLES 3

// Byte offset within VBTBKR[]. Offset 0 is the bootloader's magic.
#define VBT_WORD(off)     (*(volatile uint32_t *) &R_SYSTEM->VBTBKR[(off)])

void fault_report(uint32_t *frame) {
    muscle_sr_emergency_stop();   // before anything else (also stops the frame timer): the shift register holds its last pattern through the blinks and the reset
    // Exception stack frame: r0 r1 r2 r3 r12 lr pc xpsr
    uint32_t lr   = frame[5];
    uint32_t pc   = frame[6];
    uint32_t psr  = frame[7];
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;

    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;

    VBT_WORD(4)  = FAULT_MARKER;
    VBT_WORD(8)  = pc;
    VBT_WORD(12) = lr;
    VBT_WORD(16) = psr;
    VBT_WORD(20) = cfsr;
    VBT_WORD(24) = hfsr;

    R_SYSTEM->PRCR = PRCR_LOCK;

    for (int cycle = 0; cycle < FAULT_BLINK_CYCLES; cycle++) {
        for (int i = 0; i < 20; i++) blink();
        for (volatile uint32_t w = 0; w < 8000000; w++) { __asm volatile ("nop"); }
    }

    NVIC_SystemReset();
}

// Naked stubs: pick the stack the frame was pushed onto (MSP or PSP, per bit 2
// of EXC_RETURN in LR) and hand it to the C reporter.
#define FAULT_STUB(name)                        \
    __attribute__((naked)) void name(void) {    \
        __asm volatile (                        \
            "tst   lr, #4            \n"        \
            "ite   eq                \n"        \
            "mrseq r0, msp           \n"        \
            "mrsne r0, psp           \n"        \
            "b     fault_report      \n"        \
        );                                      \
    }

FAULT_STUB(HardFault_Handler)
FAULT_STUB(MemManage_Handler)
FAULT_STUB(BusFault_Handler)
FAULT_STUB(UsageFault_Handler)
