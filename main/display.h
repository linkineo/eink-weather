#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include "weather.h"

// Power the panel rail and configure the driver GPIOs. Call once at boot.
void display_init(void);

// Redraw the whole panel (full refresh) from the latest data, then put the
// panel controllers to sleep. Layout: ui.c.
void display_render(const weather_t *w, const status_t *st);

// Switch the panel supply off and hold it off through deep sleep.
void display_power_off(void);

#endif // _DISPLAY_H_
