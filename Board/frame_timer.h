// Board/frame_timer.h — GPT7 as the muscle frame timer for Robot/limbs/muscle/muscle_sr.

#ifndef BOARD_FRAME_TIMER_H
#define BOARD_FRAME_TIMER_H

// frame_timer_bind — open GPT7 (stopped), and hand the timer to muscle_sr.
// Call before muscle_sr_init(), which starts it.
void frame_timer_bind(void);

#endif
