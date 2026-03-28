idf_component_register(
    SRCS "cardputer_display.c" "cardputer_keyboard.c" "cardputer_keymap.c" "cardputer_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES apple2 driver esp_lcd
)
