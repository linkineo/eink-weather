idf_component_register(
    SRCS "spi.c" "EPD_Init.c" "EPD.c" "epd_ext.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_rom
)
