#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "apple2/apple2_video.h"

#include "esp_err.h"
#include "esp_lcd_types.h"

typedef struct {
    uint16_t native_width;
    uint16_t native_height;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    bool graphics_map_valid;
    uint16_t graphics_src_width;
    uint16_t graphics_src_height;
    uint16_t graphics_offset_x;
    uint16_t graphics_offset_y;
    uint16_t graphics_out_width;
    uint16_t graphics_out_height;
    uint16_t graphics_src_x[CONFIG_M5APPLE2_LCD_WIDTH];
    uint16_t graphics_src_y[CONFIG_M5APPLE2_LCD_HEIGHT];
} cardputer_display_t;

esp_err_t cardputer_display_init(cardputer_display_t *display);
esp_err_t cardputer_display_present_apple2_text40(cardputer_display_t *display,
                                                  const uint8_t *memory,
                                                  const apple2_video_state_t *state);
esp_err_t cardputer_display_present_apple2(cardputer_display_t *display,
                                           const uint8_t *pixels,
                                           uint16_t width,
                                           uint16_t height);
