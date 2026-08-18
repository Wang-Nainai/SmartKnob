idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS include
                       REQUIRES esp_lcd driver lvgl__lvgl atanisoft__esp_lcd_touch_xpt2046)
