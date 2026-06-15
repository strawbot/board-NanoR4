#ifndef USB_INIT_H
#define USB_INIT_H

// Call once from hal_entry() after init_tea() and usart_transport_init().
// Initialises TinyUSB, registers usb_action in the tea.c queue, and
// presents a DFU Runtime interface on the USB port.
void usb_transport_init(void);

#endif // USB_INIT_H
