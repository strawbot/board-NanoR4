// usb_init_stubs.c — stand-ins for usb_init.c's CLI words, compiled ONLY
// into env:nano_r4_usbtest (excluded from env:nano_r4 in platformio.ini,
// where usb_init.c itself provides the real implementations).
//
// boardwords.txt is shared between both environments, so the generated
// wordlist.c always references usbisr/usbinit/usbcrash/usbtrace
// regardless of which USB stack is actually built in. usb_init.c (and its
// TinyUSB dependency) is excluded from this environment entirely — see
// Docs/usb-handoff.md, "Native stack smoke test" — so these words need
// something to link against here. Use `nusbisr`/`nusbevt`
// (usb_native_test.c) for this build's real diagnostics instead.

#include "hal_data.h"
#include "printers.h"

static void usb_init_stub_message(void) {
    print("not built into this firmware — see env:nano_r4_usbtest's nusbisr/nusbevt"); printCr();
}

void usb_cli_isr_count(void)     { usb_init_stub_message(); }
void usb_cli_init_status(void)   { usb_init_stub_message(); }
void usb_cli_crash_snapshot(void) { usb_init_stub_message(); }
void usb_cli_trace_dump(void)    { usb_init_stub_message(); }

// The `dfu` CLI word (boardwords.txt) maps directly to this name, which is
// normally TinyUSB's DFU Runtime host-triggered callback (usb_init.c). Kept
// working here — not stubbed — as a real bootloader-entry shortcut so this
// throwaway test build can still be reflashed over dfu-util without a
// physical double-tap reset. Minimal version of usb_init.c's
// dfu_arm_bootloader()/reboot_to_dfu_action(): no DFU_DETACH-race guard,
// since this build never runs a live DFU session for one to race against.
#define PRCR_PRC1_UNLOCK  0xA502U
#define PRCR_LOCK         0xA500U
#define VBTBER_VBAE       (1U << 3)
#define DFU_MAGIC         0x07738135UL
#define DFU_BOOT_FLAG     (*(volatile uint32_t *) &R_SYSTEM->VBTBKR[0])

void tud_dfu_runtime_reboot_to_dfu_cb(void) {
    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;
    DFU_BOOT_FLAG    = DFU_MAGIC;
    R_SYSTEM->PRCR   = PRCR_LOCK;

    print("dfu: entering bootloader..."); printCr();
    NVIC_SystemReset();
}
