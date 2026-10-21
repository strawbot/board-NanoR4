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

// usb_trace_init/usb_trace — append-only checkpoint log for the
// SET_ADDRESS/enumeration stall investigation, called from patched
// checkpoints in the tinyusb submodule. usb_trace_init() resets the log
// for a fresh boot; usb_trace() appends one entry. See Docs/usb-handoff.md.
void usb_trace_init(void);
void usb_trace(uint32_t checkpoint_id, uint32_t extra);

// CLI word `usbtrace` — dump the checkpoint log captured by usb_trace().
// See Docs/usb-handoff.md.
void usb_cli_trace_dump(void);


#endif // USB_INIT_H
