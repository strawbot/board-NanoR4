// usb_native_test.c — native r_usb_basic + r_usb_pcdc smoke test.
// See usb_native_test.h and Docs/usb-handoff.md, "Native stack smoke test".
//
// Everything here compiles in BOTH environments (this file is not excluded
// from nano_r4's build_src_filter, so its CLI words always resolve), but
// the bodies that actually touch r_usb_basic are behind USE_NATIVE_USB_STACK
// (only defined for env:nano_r4_usbtest) — in env:nano_r4 these are inert
// stubs, referencing nothing from r_usb_basic (which isn't even compiled
// there), so this file can never be what breaks the working TinyUSB build.

#include <stdint.h>
#include <stdbool.h>

#include "ttypes.h"
#include "printers.h"

#include "usb_native_test.h"

#ifdef USE_NATIVE_USB_STACK

#include "hal_data.h"

// usb_native_test_descriptor.c's definition — no FSP header declares this
// (RASC's own template doesn't either; it's meant to be consumed only via
// the g_basic0_cfg it's wired into by name, which is exactly what we're
// replicating by hand here).
extern const usb_descriptor_t g_usb_descriptor;

// r_usb_basic's own ISR body (r_usb_mcu.c) -- like usbfs_int_isr in
// vector_data.c, referenced there with no header declaration either, so
// follow the same (implicit-declaration) precedent already established in
// this codebase rather than introducing a new pattern for just this file.
extern void usbfs_interrupt_handler(void);

// ── ISR fire counter ─────────────────────────────────────────────────────────
//
// usbfs_interrupt_handler() is r_usb_basic's own ISR body (r_usb_mcu.c) --
// wired at vector slot 6 in ra_gen/vector_data.c when USE_NATIVE_USB_STACK
// is defined, exactly mirroring how usb_init.c's usbfs_int_isr() wraps
// tud_int_handler() for the TinyUSB build. Counting real ISR entries here,
// completely independent of any polling, is the whole point of this test.
static volatile uint32_t native_isr_count;

void usbfs_native_isr(void) {
    native_isr_count++;
    usbfs_interrupt_handler();
}

void usb_native_cli_isr_count(void) {
    print("usbfs_interrupt_handler fired "); printDec((Long) native_isr_count);
    print(" times"); printCr();
}

// ── Application callback — one entry per USB state transition ───────────────
//
// r_usb_basic calls this directly from ISR/driver context on every state
// change (Powered/Default/Address/Configured/Suspend/Resume/...), giving a
// coarser but zero-ambiguity signal than raw ISR counting: if this stalls
// at, say, USB_STATUS_ADDRESS while a real host keeps retrying, that is the
// native stack hitting the same wall TinyUSB did. If it reaches
// USB_STATUS_CONFIGURED and keeps taking REQUEST/READ/WRITE_COMPLETE events
// under continued traffic, the native driver's interrupt path is reliable
// and the bug is specific to our TinyUSB integration, not the silicon.
static volatile uint32_t native_event_count;
static volatile uint16_t native_last_event;

static void usb_native_callback(usb_callback_args_t *p_args) {
    native_event_count++;
    native_last_event = (uint16_t) p_args->event;
}

void usb_native_cli_event_count(void) {
    print("callback fired "); printDec((Long) native_event_count); print(" times");
    print(", last event="); printDec((Long) native_last_event); printCr();
}

// Board patch: RASC generated g_basic0_cfg with p_usb_apl_callback = NULL
// (never wired up, since nothing ever called R_USB_Open against it). Hook
// our counter in instead of hand-editing the generated hal_data.c.
static const usb_cfg_t usb_native_test_cfg = {
    .usb_mode           = USB_MODE_PERI,
    .usb_speed          = USB_SPEED_FS,
    .module_number      = 0,
    .type               = USB_CLASS_PCDC,
    .p_usb_reg          = (usb_descriptor_t *) &g_usb_descriptor,
    .usb_complience_cb  = NULL,
    .irq                = VECTOR_NUMBER_USBFS_INT,
    .irq_r              = FSP_INVALID_VECTOR,
    .irq_d0             = FSP_INVALID_VECTOR,
    .irq_d1             = FSP_INVALID_VECTOR,
    .hsirq              = FSP_INVALID_VECTOR,
    .hsirq_d0           = FSP_INVALID_VECTOR,
    .hsirq_d1           = FSP_INVALID_VECTOR,
    .ipl                = (12),
    .ipl_r              = (12),
    .ipl_d0             = (12),
    .ipl_d1             = (12),
    .hsipl              = (BSP_IRQ_DISABLED),
    .hsipl_d0           = (BSP_IRQ_DISABLED),
    .hsipl_d1           = (BSP_IRQ_DISABLED),
    .p_usb_apl_callback = usb_native_callback,
    .p_context          = NULL,
};

static usb_instance_ctrl_t usb_native_test_ctrl;

void usb_native_test_init(void) {
    fsp_err_t err = R_USB_Open((usb_ctrl_t *) &usb_native_test_ctrl, &usb_native_test_cfg);
    print("R_USB_Open returned "); printDec((Long) err); printCr();

    // Board patch: TinyUSB's own dcd_init() doesn't set this either — it
    // was hand-added to usb_init.c after discovering that without it,
    // SYSCFG.DPRPU can read back "set" in software while the actual D+
    // pull-up drive is electrically marginal (VDCEN enables the
    // transceiver's regulator). r_usb_basic's R_USB_Open() doesn't set it
    // any more than TinyUSB's driver does, so replicate the same fix here.
    // See Docs/usb-handoff.md.
    R_USB_FS0->USBMC |= R_USB_FS0_USBMC_VDCEN_Msk;
}

#else // !USE_NATIVE_USB_STACK

void usb_native_test_init(void) {
    print("usb_native_test: not built into this firmware — see env:nano_r4_usbtest"); printCr();
}

void usb_native_cli_isr_count(void) {
    print("usb_native_test: not built into this firmware — see env:nano_r4_usbtest"); printCr();
}

void usb_native_cli_event_count(void) {
    print("usb_native_test: not built into this firmware — see env:nano_r4_usbtest"); printCr();
}

#endif // USE_NATIVE_USB_STACK
