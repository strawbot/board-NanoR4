// postmortem_hw.c — reset-surviving storage for Robot/diagnostics/postmortem.
//
// The record lives in the VBATT backup registers (VBTBKR), which keep their
// contents through any reset while VCC stays up (see fault.c, which stashes
// HardFault details in the same place, and why VBTBER.VBAE has to be set first).
//
//   VBTBKR bytes  0.. 3   the bootloader's double-tap magic
//                 4..27   fault.c (HardFault PC/LR/xPSR/CFSR/HFSR)
//                32..55   usb_isr_snapshot() in usb_init.c (USB storm capture)
//                64..103  this record (POSTMORTEM_WORDS 32-bit words)
//
// Nothing here is read back through the bootloader, so a board put into DFU
// mode and reflashed loses nothing it was not already going to lose: the record
// is rewritten at every boot, after the previous one is saved for show-postmortem.

#include "hal_data.h"
#include "postmortem.h"
#include "postmortem_hw.h"
#include "tea.h"

#define PM_BASE_BYTE      64U
#define PRCR_PRC1_UNLOCK  0xA502U
#define PRCR_LOCK         0xA500U
#define VBTBER_VBAE       (1U << 3)

// PRCR stays unlocked until the data access is done, as fault.c and the USB
// snapshot do: locking straight after the VBTBER write left the backup registers
// ignoring the access that followed it.
static volatile uint32_t *word(unsigned i) {
    return (volatile uint32_t *) &R_SYSTEM->VBTBKR[PM_BASE_BYTE + 4U * i];
}

static uint32_t pm_load(unsigned i) {
    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;
    uint32_t v = *word(i);
    R_SYSTEM->PRCR   = PRCR_LOCK;
    return v;
}

static void pm_store(unsigned i, uint32_t v) {
    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;
    *word(i) = v;
    R_SYSTEM->PRCR   = PRCR_LOCK;
}

// RSTSR0 | RSTSR1 << 8 | RSTSR2 << 24, then cleared so the next reset's flags are
// not mixed with this one's (PORF, for one, stays set until written to 0).
static uint32_t pm_reset_flags(void) {
    uint32_t flags = (uint32_t) R_SYSTEM->RSTSR0 | ((uint32_t) R_SYSTEM->RSTSR1 << 8) | ((uint32_t) R_SYSTEM->RSTSR2 << 24);
    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->RSTSR0 = 0;
    R_SYSTEM->RSTSR1 = 0;
    R_SYSTEM->RSTSR2 = 0;
    R_SYSTEM->PRCR   = PRCR_LOCK;
    return flags;
}

static const postmortem_hw_t ops = { pm_load, pm_store, pm_reset_flags };

// Deliberately reach BLACK_HOLE() to prove the record: the board stops (no LED,
// no console) until reset, then show-postmortem reports reason 99.
void postmortem_test(void) { BLACK_HOLE(99); }

void postmortem_hw_init(void) {
    postmortem_bind_hw(&ops);
    postmortem_init();
}
