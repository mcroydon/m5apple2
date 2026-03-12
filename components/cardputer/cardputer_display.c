#include "cardputer/cardputer_display.h"

esp_err_t cardputer_display_init(cardputer_display_t *display)
{
    display->native_width = CONFIG_M5APPLE2_LCD_WIDTH;
    display->native_height = CONFIG_M5APPLE2_LCD_HEIGHT;
    return ESP_OK;
}

