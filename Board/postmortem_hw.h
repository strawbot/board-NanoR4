#ifndef POSTMORTEM_HW_H
#define POSTMORTEM_HW_H

// Binds Robot/diagnostics/postmortem to the RA4M1's VBATT backup registers and
// starts it. Call after init_tea().
void postmortem_hw_init(void);
void postmortem_test(void);

#endif
