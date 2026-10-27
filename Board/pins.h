// Board/pins.h — pin introspection and manual GPIO control for bench debug.
// See Board/pins.c for the Pxyy numbering convention and what each word does.

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

void pins_cli_show(void);      // CLI word: pins    [filter] -- dump known pins' live PFS state
void pins_cli_read(void);      // CLI word: pin@     ( p -- level )   read a pin's input level
void pins_cli_dir_set(void);   // CLI word: pindir!  ( dir p -- )     claim pin as GPIO; dir 0=in, 1=out
void pins_cli_set(void);       // CLI word: pinset!  ( level p -- )   write a GPIO-output pin's level

#endif
