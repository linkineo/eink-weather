// Post-mortem diagnostics for unattended operation.
//
// The current cycle stage lives in RTC memory (survives watchdog/software
// resets and deep sleep); the last "went to sleep" record lives in NVS
// (survives power loss). On the next boot this tells a hang (restart while in
// a stage) apart from a power cut (expected wake time long passed).
#ifndef _DIAG_H_
#define _DIAG_H_

#include <time.h>

typedef enum {
    DIAG_BOOT = 1,
    DIAG_WIFI,
    DIAG_SNTP,
    DIAG_FETCH,
    DIAG_PANEL,
    DIAG_SLEEP,
} diag_stage_t;

// Log why we booted and what the previous run was doing; arm the cycle watchdog
// (restart if one wake cycle takes longer than CYCLE_WATCHDOG_S).
void diag_boot(void);

void diag_stage(diag_stage_t stage);

// After the clock is valid: report wakes missed since the last recorded sleep.
void diag_check_missed(time_t now);

// Record that we go to sleep now and expect to wake at `wake`.
void diag_sleeping(time_t now, time_t wake);

#endif // _DIAG_H_
