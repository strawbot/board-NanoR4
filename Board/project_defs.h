
#ifndef PROJECT_DEFS_H_
#define PROJECT_DEFS_H_

#include "ttypes.h"
#include "bsp_api.h"
#include "cmsis_gcc.h"
#include "core_cm4.h"

// bigger buffer for accepting long hexscii sequences
#define CLI_PARAMETERS

#define CLI_TITLE "ActiveRobot Nano\n"

#define DCELLS 20  // number of data stack cells
#define RCELLS 20  // number of return stack cells
#define LINE_LENGTH 400 // number of characters allowed in tib
#define EMITQ_SIZE 400
#define KEYQ_SIZE 400
#define PAD_SIZE 20
#define PROMPTSTRING "ar: "
#define CUSHION LINE_LENGTH // how much space to maintain for HERE
#define HERE_SPACE 5000 // small here space
#define OUTPUT_BLOCKED output() // deal with by running machines
#define OUTPUT_FLUSH output()
#define FLOAT_SUPPORT true
#define NAN (__builtin_nanf(""))

void output_flush();
void output();

// interrupt support
// max for PPS timing; hi for time events; lo for uarts; min for dma rings
#define INT_MAX_PRIO    0
#define INT_HI_PRIO     1
#define INT_LO_PRIO     2
#define INT_MIN_PRIO    3
#define in_interrupt()	(__get_IPSR() != 0)

// UTC and time event clocks
// RA4M1 ICLK = 32 MHz (XTAL 16 MHz × 8 / 2 / 2 via PLL; see bsp_clock_cfg.h).
#define CLOCK_MHZ 32u
#define ONE_SECOND (31250)	// PCLKD/1024 @ 32 MHz = 31.25 kHz → 32 µs/tick
#define TE_SECOND ONE_SECOND    // for Delta timer
// Hardware RTC via FSP; implementation in Board/clocks.c.
Long tick_get_utc(void);
#define get_utc() tick_get_utc()

// ── Clocks hardware wiring (consumed by TimbreOS/clocks.c) ────────────────
// Nano uses FSP GPT/RTC APIs — all hardware clock functions live in
// Board/clocks.c.  TimbreOS/clocks.c supplies only the board-agnostic
// utilities (timestamp_to_utc, epoch_to_tm, tm_to_epoch, over_due,
// micro_sleep, print_build_banner, show_timer).
#define CLOCK_HAS_BLINK   // Board/clocks.c provides blink_leds() via FSP IOPORT
#define CLOCK_HAS_INIT    // Board/clocks.c provides init_clocks() via FSP GPT
#define CLOCK_HAS_TICKS   // Board/clocks.c provides get_ticks() via FSP GPT
#define CLOCK_HAS_DELTA   // Board/clocks.c provides set_delta_alarm() / delta_alarm()

// Hi res time measurements
// 32 MHz DWT cycle counter ticks; 31.25 ns resolution
#define sysTicks()  (Long)(DWT->CYCCNT)

#define SYS_TO_NS(n) ((unsigned long long)(n)*1000/CLOCK_MHZ)
#define SYS_TO_US(n) ((n)/CLOCK_MHZ)
#define SYS_TO_MS(n) ((unsigned long long)(n)/(CLOCK_MHZ*1000))
#define US_TO_SYS(n)	 ((n)*CLOCK_MHZ)

#define IN_INTERRUPT() 	(SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk)

#define ENTER_REGION()                                          \
	{                                                           \
		uint32_t primask_bit = __get_PRIMASK();					\
		__disable_irq();

#define LEAVE_REGION()                                          \
		  __set_PRIMASK(primask_bit);                           \
	}

#define ENTER_SAFE_REGION() ENTER_REGION()
#define LEAVE_SAFE_REGION() LEAVE_REGION()

// Record event parameters
#define NUM_ACTIONS 80
#define NUM_TE 80

#define N_EVENTS 400
#define FIRST_EVENT (const char *)secs(5)

// define space for action stats
#define TEA_TABLE HASH8

// black hole reasons - addendum
#define DMA_OVERBOOKED 7

// user flow control
void user_monitor();
bool user_stop();
void user_return();

#define DEFAULT_SLEEP_MODE 1

/*!
 * \brief Convert CPU cycles into micro-seconds.
 *
 * \param  cy:      Number of CPU cycles.
 * \param  fcpu_hz: CPU frequency in Hz.
 *
 * \return the converted number of micro-second.
 */
#define cpu_cy_2_us(cy, fcpu_hz) (((Octet)(cy) * 1000000 + (fcpu_hz)-1) / (fcpu_hz))

void blink(); // for debugging; insert to create a single LED blink

#endif
