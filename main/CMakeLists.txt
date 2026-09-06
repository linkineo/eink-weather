idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES gfx epd_uc8179
                       PRIV_REQUIRES spi_flash esp_timer)
