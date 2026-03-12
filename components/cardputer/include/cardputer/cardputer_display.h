#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t native_width;
    uint16_t native_height;
} cardputer_display_t;

esp_err_t cardputer_display_init(cardputer_display_t *display);

