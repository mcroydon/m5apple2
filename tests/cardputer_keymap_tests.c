#include "cardputer/cardputer_keymap.h"
#include "cardputer/cardputer_input.h"

#include <assert.h>
#include <stdio.h>

static uint64_t mask_for(uint8_t row, uint8_t column)
{
    return cardputer_keymap_mask_for_coord((cardputer_keycoord_t){
        .row = row,
        .column = column,
    });
}

static void test_original_decode(void)
{
    cardputer_keycoord_t coord;

    assert(cardputer_keymap_decode_original(7, 0, &coord));
    assert(coord.row == 0U);
    assert(coord.column == 0U);

    assert(cardputer_keymap_decode_original(3, 0, &coord));
    assert(coord.row == 0U);
    assert(coord.column == 1U);

    assert(cardputer_keymap_decode_original(6, 6, &coord));
    assert(coord.row == 1U);
    assert(coord.column == 12U);

    assert(!cardputer_keymap_decode_original(8, 0, &coord));
    assert(!cardputer_keymap_decode_original(0, 7, &coord));
}

static void test_adv_decode(void)
{
    cardputer_keycoord_t coord;

    assert(cardputer_keymap_decode_adv(0, 0, &coord));
    assert(coord.row == 0U);
    assert(coord.column == 0U);

    assert(cardputer_keymap_decode_adv(6, 7, &coord));
    assert(coord.row == 3U);
    assert(coord.column == 13U);

    assert(!cardputer_keymap_decode_adv(7, 0, &coord));
    assert(!cardputer_keymap_decode_adv(0, 8, &coord));
}

static void test_ascii_translation(void)
{
    uint8_t ascii = 0;

    assert(cardputer_keymap_ascii_for_press(mask_for(1, 1),
                                            (cardputer_keycoord_t){ .row = 1U, .column = 1U },
                                            &ascii));
    assert(ascii == 'q');

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 1) | mask_for(1, 1),
                                            (cardputer_keycoord_t){ .row = 1U, .column = 1U },
                                            &ascii));
    assert(ascii == 'Q');

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 1) | mask_for(0, 2),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 2U },
                                            &ascii));
    assert(ascii == '@');

    assert(cardputer_keymap_ascii_for_press(mask_for(3, 0) | mask_for(2, 2),
                                            (cardputer_keycoord_t){ .row = 2U, .column = 2U },
                                            &ascii));
    assert(ascii == 0x01U);

    assert(cardputer_keymap_ascii_for_press(mask_for(3, 0) | mask_for(2, 1) | mask_for(0, 2),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 2U },
                                            &ascii));
    assert(ascii == 0x00U);

    assert(cardputer_keymap_ascii_for_press(mask_for(0, 13),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 13U },
                                            &ascii));
    assert(ascii == 0x08U);

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 13),
                                            (cardputer_keycoord_t){ .row = 2U, .column = 13U },
                                            &ascii));
    assert(ascii == '\r');
}

static void test_modifiers_do_not_emit_ascii(void)
{
    uint8_t ascii = 0;

    assert(cardputer_keymap_is_modifier((cardputer_keycoord_t){ .row = 2U, .column = 0U }));
    assert(cardputer_keymap_is_modifier((cardputer_keycoord_t){ .row = 2U, .column = 1U }));
    assert(cardputer_keymap_is_modifier((cardputer_keycoord_t){ .row = 3U, .column = 0U }));
    assert(!cardputer_keymap_ascii_for_press(mask_for(2, 1),
                                             (cardputer_keycoord_t){ .row = 2U, .column = 1U },
                                             &ascii));
}

static void test_fn_disk_commands(void)
{
    uint8_t ascii = 0;

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 0) | mask_for(0, 1),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 1U },
                                            &ascii));
    assert(ascii == CARDPUTER_INPUT_CMD_SD_DRIVE1);

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 0) | mask_for(0, 2),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 2U },
                                            &ascii));
    assert(ascii == CARDPUTER_INPUT_CMD_SD_DRIVE2);

    assert(cardputer_keymap_ascii_for_press(mask_for(2, 0) | mask_for(0, 10),
                                            (cardputer_keycoord_t){ .row = 0U, .column = 10U },
                                            &ascii));
    assert(ascii == CARDPUTER_INPUT_CMD_SD_RESCAN);
}

int main(void)
{
    test_original_decode();
    test_adv_decode();
    test_ascii_translation();
    test_modifiers_do_not_emit_ascii();
    test_fn_disk_commands();
    puts("cardputer keymap tests passed");
    return 0;
}
