#include "cardputer/cardputer_keymap.h"
#include "cardputer/cardputer_input.h"

typedef enum {
    CARDPUTER_KEY_KIND_NONE = 0,
    CARDPUTER_KEY_KIND_CHAR,
    CARDPUTER_KEY_KIND_BACKSPACE,
    CARDPUTER_KEY_KIND_TAB,
    CARDPUTER_KEY_KIND_ENTER,
    CARDPUTER_KEY_KIND_FN,
    CARDPUTER_KEY_KIND_SHIFT,
    CARDPUTER_KEY_KIND_CTRL,
    CARDPUTER_KEY_KIND_OPT,
    CARDPUTER_KEY_KIND_ALT,
} cardputer_key_kind_t;

typedef struct {
    cardputer_key_kind_t kind;
    uint8_t normal;
    uint8_t shifted;
} cardputer_keydef_t;

#define CARDPUTER_CHAR(a, b) { CARDPUTER_KEY_KIND_CHAR, (uint8_t)(a), (uint8_t)(b) }
#define CARDPUTER_SPECIAL(kind) { (kind), 0U, 0U }

static const uint8_t s_original_column_pairs[7][2] = {
    { 0U, 1U },
    { 2U, 3U },
    { 4U, 5U },
    { 6U, 7U },
    { 8U, 9U },
    { 10U, 11U },
    { 12U, 13U },
};

static const cardputer_keydef_t s_keymap[CARDPUTER_KEYMAP_ROWS][CARDPUTER_KEYMAP_COLUMNS] = {
    {
        CARDPUTER_CHAR('`', '~'),
        CARDPUTER_CHAR('1', '!'),
        CARDPUTER_CHAR('2', '@'),
        CARDPUTER_CHAR('3', '#'),
        CARDPUTER_CHAR('4', '$'),
        CARDPUTER_CHAR('5', '%'),
        CARDPUTER_CHAR('6', '^'),
        CARDPUTER_CHAR('7', '&'),
        CARDPUTER_CHAR('8', '*'),
        CARDPUTER_CHAR('9', '('),
        CARDPUTER_CHAR('0', ')'),
        CARDPUTER_CHAR('-', '_'),
        CARDPUTER_CHAR('=', '+'),
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_BACKSPACE),
    },
    {
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_TAB),
        CARDPUTER_CHAR('q', 'Q'),
        CARDPUTER_CHAR('w', 'W'),
        CARDPUTER_CHAR('e', 'E'),
        CARDPUTER_CHAR('r', 'R'),
        CARDPUTER_CHAR('t', 'T'),
        CARDPUTER_CHAR('y', 'Y'),
        CARDPUTER_CHAR('u', 'U'),
        CARDPUTER_CHAR('i', 'I'),
        CARDPUTER_CHAR('o', 'O'),
        CARDPUTER_CHAR('p', 'P'),
        CARDPUTER_CHAR('[', '{'),
        CARDPUTER_CHAR(']', '}'),
        CARDPUTER_CHAR('\\', '|'),
    },
    {
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_FN),
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_SHIFT),
        CARDPUTER_CHAR('a', 'A'),
        CARDPUTER_CHAR('s', 'S'),
        CARDPUTER_CHAR('d', 'D'),
        CARDPUTER_CHAR('f', 'F'),
        CARDPUTER_CHAR('g', 'G'),
        CARDPUTER_CHAR('h', 'H'),
        CARDPUTER_CHAR('j', 'J'),
        CARDPUTER_CHAR('k', 'K'),
        CARDPUTER_CHAR('l', 'L'),
        CARDPUTER_CHAR(';', ':'),
        CARDPUTER_CHAR('\'', '"'),
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_ENTER),
    },
    {
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_CTRL),
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_OPT),
        CARDPUTER_SPECIAL(CARDPUTER_KEY_KIND_ALT),
        CARDPUTER_CHAR('z', 'Z'),
        CARDPUTER_CHAR('x', 'X'),
        CARDPUTER_CHAR('c', 'C'),
        CARDPUTER_CHAR('v', 'V'),
        CARDPUTER_CHAR('b', 'B'),
        CARDPUTER_CHAR('n', 'N'),
        CARDPUTER_CHAR('m', 'M'),
        CARDPUTER_CHAR(',', '<'),
        CARDPUTER_CHAR('.', '>'),
        CARDPUTER_CHAR('/', '?'),
        CARDPUTER_CHAR(' ', ' '),
    },
};

static bool cardputer_keymap_coord_valid(cardputer_keycoord_t coord)
{
    return coord.row < CARDPUTER_KEYMAP_ROWS && coord.column < CARDPUTER_KEYMAP_COLUMNS;
}

static const cardputer_keydef_t *cardputer_keymap_lookup(cardputer_keycoord_t coord)
{
    if (!cardputer_keymap_coord_valid(coord)) {
        return 0;
    }
    return &s_keymap[coord.row][coord.column];
}

static bool cardputer_keymap_modifier_active(uint64_t pressed_mask, cardputer_keycoord_t coord)
{
    return (pressed_mask & cardputer_keymap_mask_for_coord(coord)) != 0U;
}

static bool cardputer_keymap_ctrl_modifier_active(uint64_t pressed_mask)
{
    return cardputer_keymap_modifier_active(pressed_mask,
                                            (cardputer_keycoord_t){ .row = 3U, .column = 0U });
}

static bool cardputer_keymap_command_modifier_active(uint64_t pressed_mask)
{
    if (cardputer_keymap_modifier_active(pressed_mask,
                                         (cardputer_keycoord_t){ .row = 2U, .column = 0U })) {
        return true;
    }

    return false;
}

static bool cardputer_keymap_fn_ascii_for_char(uint8_t normal, uint8_t *ascii)
{
    switch (normal) {
    case '`':
        *ascii = 0x1BU; /* ESC */
        return true;
    case '0':
        *ascii = CARDPUTER_INPUT_CMD_SD_RESCAN;
        return true;
    case '1':
        *ascii = CARDPUTER_INPUT_CMD_SD_DRIVE1;
        return true;
    case '2':
        *ascii = CARDPUTER_INPUT_CMD_SD_DRIVE2;
        return true;
    case '3':
        *ascii = CARDPUTER_INPUT_CMD_SD_ORDER1;
        return true;
    case '4':
        *ascii = CARDPUTER_INPUT_CMD_SD_ORDER2;
        return true;
    case '5':
        *ascii = CARDPUTER_INPUT_CMD_SD_PICKER1;
        return true;
    case '6':
        *ascii = CARDPUTER_INPUT_CMD_SD_PICKER2;
        return true;
    case '7':
        *ascii = CARDPUTER_INPUT_CMD_BOOT_SLOT6;
        return true;
    case '9':
        *ascii = CARDPUTER_INPUT_CMD_RESET_BOOT_SLOT6;
        return true;
    case '8':
        *ascii = CARDPUTER_INPUT_CMD_SPEED_TOGGLE;
        return true;
    case 'i':
    case ';':
        *ascii = 0x0BU; /* Up */
        return true;
    case 'j':
    case ',':
        *ascii = 0x08U; /* Left */
        return true;
    case 'k':
    case '.':
        *ascii = 0x0AU; /* Down */
        return true;
    case 'l':
    case '/':
        *ascii = 0x15U; /* Right */
        return true;
    default:
        return false;
    }
}

bool cardputer_keymap_has_fn_command(cardputer_keycoord_t coord)
{
    const cardputer_keydef_t *key = cardputer_keymap_lookup(coord);
    uint8_t ascii = 0;

    if (key == 0 || key->kind != CARDPUTER_KEY_KIND_CHAR) {
        return false;
    }

    return cardputer_keymap_fn_ascii_for_char(key->normal, &ascii);
}

bool cardputer_keymap_decode_original(uint8_t select_index,
                                      uint8_t input_index,
                                      cardputer_keycoord_t *coord)
{
    if (coord == 0 || select_index >= 8U || input_index >= 7U) {
        return false;
    }

    coord->row = (uint8_t)(3U - (select_index & 0x03U));
    coord->column = s_original_column_pairs[input_index][(select_index > 3U) ? 0U : 1U];
    return true;
}

bool cardputer_keymap_decode_adv(uint8_t row_index, uint8_t col_index, cardputer_keycoord_t *coord)
{
    if (coord == 0 || row_index >= 7U || col_index >= 8U) {
        return false;
    }

    return cardputer_keymap_decode_original((uint8_t)(7U - col_index), row_index, coord);
}

bool cardputer_keymap_decode_adv_event(uint8_t event,
                                       bool *pressed,
                                       cardputer_keycoord_t *coord)
{
    const uint8_t code = (uint8_t)(event & 0x7FU);
    const uint8_t row = (uint8_t)((code - 1U) / 10U);
    const uint8_t col = (uint8_t)((code - 1U) % 10U);

    if (pressed == 0 || coord == 0 || code == 0U) {
        return false;
    }

    /* TCA8418 KEY_EVENT bit 7 is set for press and clear for release. */
    *pressed = (event & 0x80U) != 0U;
    return cardputer_keymap_decode_adv(row, col, coord);
}

bool cardputer_keymap_coord_from_index(uint8_t index, cardputer_keycoord_t *coord)
{
    if (coord == 0 || index >= CARDPUTER_KEYMAP_KEYS) {
        return false;
    }

    coord->row = (uint8_t)(index / CARDPUTER_KEYMAP_COLUMNS);
    coord->column = (uint8_t)(index % CARDPUTER_KEYMAP_COLUMNS);
    return true;
}

bool cardputer_keymap_is_modifier(cardputer_keycoord_t coord)
{
    const cardputer_keydef_t *key = cardputer_keymap_lookup(coord);

    if (key == 0) {
        return false;
    }

    switch (key->kind) {
    case CARDPUTER_KEY_KIND_FN:
    case CARDPUTER_KEY_KIND_SHIFT:
    case CARDPUTER_KEY_KIND_CTRL:
    case CARDPUTER_KEY_KIND_OPT:
    case CARDPUTER_KEY_KIND_ALT:
        return true;
    default:
        return false;
    }
}

uint64_t cardputer_keymap_mask_for_coord(cardputer_keycoord_t coord)
{
    if (!cardputer_keymap_coord_valid(coord)) {
        return 0U;
    }

    return 1ULL << ((coord.row * CARDPUTER_KEYMAP_COLUMNS) + coord.column);
}

bool cardputer_keymap_ascii_for_press(uint64_t pressed_mask,
                                      cardputer_keycoord_t coord,
                                      uint8_t *ascii)
{
    const cardputer_keydef_t *key = cardputer_keymap_lookup(coord);
    uint8_t value = 0;
    bool shift = false;
    bool ctrl = false;
    bool fn = false;

    if (ascii == 0 || key == 0) {
        return false;
    }

    shift = cardputer_keymap_modifier_active(pressed_mask,
                                             (cardputer_keycoord_t){ .row = 2U, .column = 1U });
    ctrl = cardputer_keymap_ctrl_modifier_active(pressed_mask);
    fn = cardputer_keymap_command_modifier_active(pressed_mask);

    switch (key->kind) {
    case CARDPUTER_KEY_KIND_CHAR:
        value = shift ? key->shifted : key->normal;
        if (fn) {
            if (cardputer_keymap_has_fn_command(coord) &&
                cardputer_keymap_fn_ascii_for_char(key->normal, ascii)) {
                return true;
            }
        }
        if (ctrl) {
            if (value >= 'a' && value <= 'z') {
                value = (uint8_t)(value - 'a' + 'A');
            }
            if (value >= '@' && value <= '_') {
                value &= 0x1FU;
            }
        }
        *ascii = value;
        return true;
    case CARDPUTER_KEY_KIND_BACKSPACE:
        *ascii = 0x08U;
        return true;
    case CARDPUTER_KEY_KIND_TAB:
        *ascii = '\t';
        return true;
    case CARDPUTER_KEY_KIND_ENTER:
        *ascii = '\r';
        return true;
    default:
        return false;
    }
}
