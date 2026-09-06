/*
 * test_log.h - machine-readable log contract for automated bring-up checks.
 *
 * Every line emitted through EPD_TEST_LOG() is prefixed with "[EPD-TEST] " and
 * is parsed by tools/capture.py (markers: "[EPD-TEST] DONE" for success,
 * "[EPD-TEST] ERROR" for failure). Do not change the prefix.
 */

#pragma once

#include <stdio.h>

#define EPD_TEST_LOG(fmt, ...) printf("[EPD-TEST] " fmt "\n", ##__VA_ARGS__)
