#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CARDPUTER_KEYMAP_ROWS 4U
#define CARDPUTER_KEYMAP_COLUMNS 14U
#define CARDPUTER_KEYMAP_KEYS (CARDPUTER_KEYMAP_ROWS * CARDPUTER_KEYMAP_COLUMNS)

typedef struct {
    uint8_t row;
    uint8_t column;
} cardputer_keycoord_t;

bool cardputer_keymap_decode_original(uint8_t select_index,
                                      uint8_t input_index,
                                      cardputer_keycoord_t *coord);
bool cardputer_keymap_decode_adv(uint8_t row_index, uint8_t col_index, cardputer_keycoord_t *coord);
bool cardputer_keymap_coord_from_index(uint8_t index, cardputer_keycoord_t *coord);
bool cardputer_keymap_is_modifier(cardputer_keycoord_t coord);
uint64_t cardputer_keymap_mask_for_coord(cardputer_keycoord_t coord);
bool cardputer_keymap_ascii_for_press(uint64_t pressed_mask,
                                      cardputer_keycoord_t coord,
                                      uint8_t *ascii);
