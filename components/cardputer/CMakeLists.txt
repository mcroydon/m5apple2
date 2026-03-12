idf_component_register(
    SRCS "cardputer_display.c" "cardputer_keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd
)

