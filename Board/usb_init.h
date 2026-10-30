#ifndef USB_INIT_H
#define USB_INIT_H

// Call once from hal_entry() after init_tea() and usart_transport_init().
// Initialises TinyUSB, registers usb_action in the tea.c queue, and
// presents a DFU Runtime interface on the USB port.
void usb_transport_init(void);

// CLI word `usbisr` — print how many times usbfs_int_isr has fired.
// See Docs/usb-handoff.md.
void usb_cli_isr_count(void);

// CLI word `usbinit` — print whether tusb_init() reported success, and what
// INTENB0 held immediately after it returned, from this boot. See
// Docs/usb-handoff.md.
void usb_cli_init_status(void);

// CLI word `usbcrash` — print the ISR count / INTSTS0:INTENB0 / emitq depth
// captured by usb_isr_snapshot() before the last hang, if any. Survives a
// power cycle (VBATT backup registers). See Docs/usb-handoff.md.
void usb_cli_crash_snapshot(void);

#endif // USB_INIT_H
