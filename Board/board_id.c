// board_id.c — which board is this? See board_id.h.

#include <stdint.h>
#include <string.h>

#include "hal_data.h"
#include "printers.h"
#include "board_id.h"
#include "clocks.h"

// Serial number = "ActiveRobot" + 3 hex digits hashed (FNV-1a) from the
// RA4M1's factory-unique 128-bit ID. It used to be the constant
// "ActiveRobot", so two boards were identical to the host (same VID/PID/
// serial) and macOS showed only one of them. A short, recognisable serial
// matters beyond that: macOS builds the CDC node name from it
// (/dev/cu.usbmodem<serial><n>), so these show up as
// /dev/cu.usbmodemActiveRobot1A22 -- easy to pick out among many ports.
// (The raw 32-digit ID as the serial was too long for macOS to use in the
// node name, which fell back to a location-based name.) 12 bits is plenty to
// tell boards on one bench apart; the full ID is not needed for that.
#define USB_SERIAL_UNIQ_DIGITS  3   // ~4000 boards before a collision matters on one bench

const char *board_serial(void) {
    static char serial[sizeof("ActiveRobot") + USB_SERIAL_UNIQ_DIGITS];
    if (serial[0] == '\0') {
        const bsp_unique_id_t *uid = R_BSP_UniqueIdGet();
        uint32_t h = 2166136261u;                       // FNV-1a
        for (unsigned i = 0; i < sizeof(uid->unique_id_bytes); i++) {
            h = (h ^ uid->unique_id_bytes[i]) * 16777619u;
        }
        memcpy(serial, "ActiveRobot", sizeof("ActiveRobot") - 1);
        h ^= h >> 12 ^ h >> 24;                         // fold so low bits carry the whole hash
        for (unsigned i = 0; i < USB_SERIAL_UNIQ_DIGITS; i++) {
            serial[sizeof("ActiveRobot") - 1 + i] = "0123456789ABCDEF"[(h >> (4 * (USB_SERIAL_UNIQ_DIGITS - 1 - i))) & 0xF];
        }
    }
    return serial;
}

#define WHOAMI_SECONDS 10

void whoami(void) {
    print(board_serial());
    print("  LED flashing fast for "); printDec(WHOAMI_SECONDS); print("s"); printCr();
    blink_identify(WHOAMI_SECONDS);
}
