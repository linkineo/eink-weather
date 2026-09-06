# Third-party code and licenses

- **Waveshare `GUI_Paint` drawing library** (`components/gfx/GUI_Paint.c`, `GUI_Paint.h`) —
  from github.com/waveshareteam/e-Paper (`RaspberryPi_JetsonNano/c/lib/GUI/`), Waveshare team.
  Permissive MIT-style license reproduced in the file headers. Only `#include` lines were adapted
  (see comments marked `eink-weather:` for any other edit).
- **Bitmap fonts `font8/12/16/20/24.c`, `fonts.h`** (`components/gfx/fonts/`, `include/fonts.h`) —
  © 2014 STMicroelectronics, BSD-3-Clause (redistributed by Waveshare in the same repository).
  License text reproduced in the file headers.
- **Panel driver sequences** (`components/epd_uc8179/epd_uc8179.c`) — transcribed from Waveshare
  `EPD_7in5b_V2.c` (V2.0, 2024-08-07) and `EPD_7in5_V2.c`, MIT-style license (Waveshare team);
  the SPI/GPIO layer and the probe are original code of this project.
- **ESP-IDF** — Apache-2.0, Espressif Systems (not vendored; used as the SDK).
