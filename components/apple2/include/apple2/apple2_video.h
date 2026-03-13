#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    APPLE2_COLOR_BLACK = 0,
    APPLE2_COLOR_MAGENTA = 1,
    APPLE2_COLOR_DARK_BLUE = 2,
    APPLE2_COLOR_VIOLET = 3,
    APPLE2_COLOR_DARK_GREEN = 4,
    APPLE2_COLOR_GRAY1 = 5,
    APPLE2_COLOR_MEDIUM_BLUE = 6,
    APPLE2_COLOR_LIGHT_BLUE = 7,
    APPLE2_COLOR_BROWN = 8,
    APPLE2_COLOR_ORANGE = 9,
    APPLE2_COLOR_GRAY2 = 10,
    APPLE2_COLOR_PINK = 11,
    APPLE2_COLOR_GREEN = 12,
    APPLE2_COLOR_YELLOW = 13,
    APPLE2_COLOR_AQUA = 14,
    APPLE2_COLOR_WHITE = 15,
};

typedef struct {
    bool text_mode;
    bool mixed_mode;
    bool page2;
    bool hires_mode;
    bool flash_state;
} apple2_video_state_t;

uint16_t apple2_text_row_address(bool page2, uint8_t row);
uint16_t apple2_hires_line_address(bool page2, uint8_t line);
uint16_t apple2_palette_rgb565(uint8_t color_index);
uint8_t apple2_text_code_to_ascii(uint8_t code, bool *inverse);
const uint8_t *apple2_ascii_font(uint8_t ascii);
void apple2_video_render(const uint8_t *memory, const apple2_video_state_t *state, uint8_t *pixels);
