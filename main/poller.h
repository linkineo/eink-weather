#ifndef _POLLER_H_
#define _POLLER_H_

// Run one wake cycle (connect, fetch, redraw) and enter deep sleep until just
// before the next refresh slot. Never returns.
void poller_run(void);

#endif // _POLLER_H_
