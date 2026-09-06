/*****************************************************************************
 * | File        :   Debug.h
 * | Function    :   Shim mapping the Waveshare Debug() macro onto ESP_LOGD().
 * |
 * | The vendored GUI_Paint.c calls Debug("...") on out-of-range input. Upstream
 * | this is a printf() guarded by a -DDEBUG make flag; here it becomes an
 * | ESP-IDF debug-level log under the "gfx" tag, so the messages are filtered by
 * | the normal log level configuration instead of a rebuild.
 * |
 * | Private to the component (PRIV_INCLUDE_DIRS): consumers of gfx.h never see
 * | this header, so it cannot clash with another vendored Debug.h.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/
#ifndef GFX_DEBUG_SHIM_H_
#define GFX_DEBUG_SHIM_H_

#include "esp_log.h"

/*
 * The vendored call sites all pass a literal format string, so the ##__VA_ARGS__
 * GNU comma-swallowing extension (used by ESP_LOGD itself) is all that is needed
 * to support the zero-argument form.
 */
#define Debug(__info, ...) ESP_LOGD("gfx", __info, ##__VA_ARGS__)

#endif /* GFX_DEBUG_SHIM_H_ */
