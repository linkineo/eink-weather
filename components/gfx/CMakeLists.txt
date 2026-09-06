# gfx -- 1-bpp framebuffer drawing library for the e-paper weather display.
#
# Vendored Waveshare GUI_Paint + STMicroelectronics ASCII fonts, plus the
# eink-weather gfx_* helpers. Pure software: no SPI, no GPIO, no panel access.

idf_component_register(
    SRCS "GUI_Paint.c"
         "gfx_text.c"
         "fonts/font8.c"
         "fonts/font12.c"
         "fonts/font16.c"
         "fonts/font20.c"
         "fonts/font24.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "shims"
    REQUIRES log)

# Warning suppressions scoped to the vendored Waveshare source, so the
# eink-weather sources keep the full ESP-IDF warning set
# (-Wall -Werror=all -Wextra ...) and the vendored file stays unmodified.
#
# -Wno-type-limits
#     GUI_Paint.c:332, in Paint_DrawPoint():
#         if(Xpoint + XDir_Num - Dot_Pixel < 0 || Ypoint + YDir_Num - Dot_Pixel < 0)
#     DOT_PIXEL is an enum with only non-negative enumerators, so GCC gives it an
#     unsigned type; the whole expression becomes unsigned and "< 0" is provably
#     false. The intended negative-coordinate guard is therefore dead code. It is
#     harmless in practice -- the value wraps to a huge UWORD and Paint_SetPixel's
#     own range check discards it -- but the warning is real, not cosmetic.
#
# -Wno-sign-compare
#     Same function, lines 330/331/339/340: int16_t loop counters compared
#     against the unsigned DOT_PIXEL enum. ESP-IDF v5.0 already passes
#     -Wno-sign-compare globally; listed here so the file stays quiet if a later
#     IDF release stops doing that.
set_source_files_properties("GUI_Paint.c" PROPERTIES
    COMPILE_OPTIONS "-Wno-type-limits;-Wno-sign-compare")
