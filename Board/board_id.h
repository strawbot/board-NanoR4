#ifndef BOARD_ID_H
#define BOARD_ID_H

// The board's name, "ActiveRobot" + 3 hex digits hashed from the RA4M1's
// factory-unique ID. It is the USB serial number too, so it is what the host
// shows (/dev/cu.usbmodemActiveRobot82C2, `dfu-util -l`, System Information).
const char *board_serial(void);

// CLI word `whoami` — print board_serial() and flash the LED fast for a few
// seconds, to tell which of several identical boards is which.
void whoami(void);

#endif // BOARD_ID_H
