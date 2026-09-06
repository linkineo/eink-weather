/*****************************************************************************
 * | File        :   esp_log.h (host test stub)
 * | Function    :   No-op replacement for the ESP-IDF logging header.
 * |
 * | Lets the gfx sources be compiled with a plain host compiler: shims/Debug.h
 * | includes "esp_log.h" and expands Debug() to ESP_LOGD(). Only ESP_LOGD is
 * | needed -- the vendored GUI_Paint.c uses nothing else, and all its call sites
 * | pass a literal format string with no variables, so discarding the arguments
 * | entirely cannot create unused-variable warnings.
 * |
 * | This file is NOT part of the firmware build; it only exists on the include
 * | path used by test/run_host_test.sh.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/
#ifndef GFX_HOST_TEST_ESP_LOG_STUB_H_
#define GFX_HOST_TEST_ESP_LOG_STUB_H_

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#endif /* GFX_HOST_TEST_ESP_LOG_STUB_H_ */
