#ifndef USB_NATIVE_TEST_H
#define USB_NATIVE_TEST_H

// usb_native_test.h — RASC's native r_usb_basic + r_usb_pcdc stack, standing
// in for TinyUSB in the nano_r4_usbtest environment ONLY, to answer one
// question: does the native driver's own interrupt path stay alive under
// real host traffic, or does it wedge the same way TinyUSB's did before the
// scheduler-poll workaround? See Docs/usb-handoff.md, "Native stack smoke
// test". Declared unconditionally so hal_entry.c can include this header in
// both environments; the functions themselves are stubbed out (behind
// USE_NATIVE_USB_STACK) everywhere except nano_r4_usbtest.

// Call once from hal_entry() in place of usb_transport_init().
void usb_native_test_init(void);

// CLI word `nusbisr` — print how many times the native usbfs_interrupt_handler
// has fired.
void usb_native_cli_isr_count(void);

// CLI word `nusbevt` — print how many application-callback events have
// fired and the most recent one's usb_status_t code.
void usb_native_cli_event_count(void);

#endif // USB_NATIVE_TEST_H
