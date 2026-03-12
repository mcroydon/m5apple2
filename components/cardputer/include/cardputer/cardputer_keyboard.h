#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t cardputer_keyboard_init(void);
bool cardputer_keyboard_poll_ascii(uint8_t *ascii);
