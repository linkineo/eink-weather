#ifndef _TIMESYNC_H_
#define _TIMESYNC_H_

#include <stdbool.h>
#include <stdint.h>

// Apply the configured POSIX TZ and start SNTP (pool.ntp.org).
void timesync_start(void);

// Block until the clock has been set by SNTP or `timeout_ms` elapses.
bool timesync_wait(uint32_t timeout_ms);

bool timesync_is_valid(void);

// Block until an actual SNTP sync completes (ignores RTC-kept time, unlike
// timesync_wait). Used after deep sleep to correct and measure RTC drift.
bool timesync_wait_sntp(uint32_t timeout_ms);

#endif // _TIMESYNC_H_
