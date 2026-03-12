#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

typedef struct {
    uint16_t native_width;
    uint16_t native_height;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
} cardputer_display_t;

esp_err_t cardputer_display_init(cardputer_display_t *display);
esp_err_t cardputer_display_present_apple2(cardputer_display_t *display,
                                           const uint8_t *pixels,
                                           uint16_t width,
                                           uint16_t height);
