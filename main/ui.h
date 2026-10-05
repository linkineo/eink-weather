// Weather screen layout ("B", French) for the 792x272 1-bit panel.
// Pure drawing code with no ESP-IDF dependency, so it also builds on the host
// for previews (tools/ui_preview.sh).
#ifndef _UI_H_
#define _UI_H_

#include <time.h>
#include "weather.h"

#define UI_W 792
#define UI_H 272

// Plot one pixel; black = 1 for ink, 0 for paper. Called only in-bounds.
typedef void (*ui_px_fn)(int x, int y, int black);

// Draw the whole screen onto a white canvas. `now` gives today's date.
void ui_render(const weather_t *w, const status_t *st, time_t now, ui_px_fn px);

#endif // _UI_H_
