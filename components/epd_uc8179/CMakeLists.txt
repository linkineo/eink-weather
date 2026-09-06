idf_component_register(SRCS "epd_bus.c" "epd_uc8179.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver
                       PRIV_REQUIRES esp_timer log)
