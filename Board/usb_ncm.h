#ifndef USB_NCM_H
#define USB_NCM_H

// IP over USB (CDC-NCM) for the ActiveRobot web page. Built only into
// env:nano_r4_web (-D WITH_USB_NCM); the CLI word is present in every build.

// Call from usb_transport_init() after tusb_init(): brings up lwIP, the NCM
// netif (192.168.7.1), the DHCP server and the HTTP server.
void usb_ncm_init(void);

// Interface MAC address as the 12-hex-digit string the NCM descriptor wants.
const char *usb_ncm_mac_string(void);

// CLI word `ncm` — link state, addresses, frame counters, HTTP connections.
void usb_ncm_cli_show(void);

#endif // USB_NCM_H
