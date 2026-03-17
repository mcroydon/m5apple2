#include "cardputer/cardputer_keyboard.h"

#include <stdio.h>

#include "cardputer/cardputer_keymap.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CARDPUTER_ESC_BUTTON_GPIO 0
#define CARDPUTER_UART_QUEUE_LENGTH 32
#define CARDPUTER_MATRIX_SCAN_PERIOD_MS 5

#define CARDPUTER_ORIGINAL_SELECT_PIN0 8
#define CARDPUTER_ORIGINAL_SELECT_PIN1 9
#define CARDPUTER_ORIGINAL_SELECT_PIN2 11

#define CARDPUTER_ADV_I2C_PORT I2C_NUM_0
#define CARDPUTER_ADV_I2C_HZ 400000U
#define CARDPUTER_ADV_ADDR 0x34
#define CARDPUTER_ADV_INT_GPIO 11
#define CARDPUTER_ADV_SDA_GPIO 8
#define CARDPUTER_ADV_SCL_GPIO 9
#define CARDPUTER_ADV_FN_CHORD_WINDOW_MS 30

#define TCA8418_REG_CFG 0x01
#define TCA8418_REG_INT_STAT 0x02
#define TCA8418_REG_KEY_LCK_EC 0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO_1 0x1D
#define TCA8418_REG_KP_GPIO_2 0x1E
#define TCA8418_REG_KP_GPIO_3 0x1F
#define TCA8418_REG_DEBOUNCE_DIS_1 0x29
#define TCA8418_REG_DEBOUNCE_DIS_2 0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3 0x2B

#define TCA8418_CFG_AUTO_INCREMENT 0x80
#define TCA8418_CFG_KEY_INTERRUPT 0x01
#define TCA8418_INT_STAT_KEY_EVENT 0x01

static const char *TAG = "cardputer_kbd";
static QueueHandle_t s_ascii_queue;
static bool s_esc_button_down;
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
static TickType_t s_last_matrix_scan;
static uint64_t s_matrix_mask;
#endif
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
static TickType_t s_last_adv_poll;
static uint64_t s_adv_pressed_mask;
static TickType_t s_adv_fn_latch_until;
static struct {
    bool active;
    cardputer_keycoord_t coord;
    TickType_t deadline;
} s_adv_pending_press;
#endif

static uint8_t cardputer_translate_input(int ch)
{
    if (ch == '\n') {
        return '\r';
    }
    if (ch == 0x7FU) {
        return 0x08U;
    }
    return (uint8_t)ch;
}

static bool cardputer_enqueue_ascii(uint8_t ascii)
{
    if (s_ascii_queue == NULL) {
        return false;
    }
    return xQueueSend(s_ascii_queue, &ascii, 0) == pdTRUE;
}

static esp_err_t cardputer_configure_input(gpio_num_t gpio)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << (uint32_t)gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

static esp_err_t cardputer_init_esc_button(void)
{
    esp_err_t err = cardputer_configure_input((gpio_num_t)CARDPUTER_ESC_BUTTON_GPIO);

    if (err != ESP_OK) {
        return err;
    }

    s_esc_button_down = gpio_get_level((gpio_num_t)CARDPUTER_ESC_BUTTON_GPIO) == 0;
    return ESP_OK;
}

static void cardputer_poll_esc_button(void)
{
    const bool down = gpio_get_level((gpio_num_t)CARDPUTER_ESC_BUTTON_GPIO) == 0;

    if (down && !s_esc_button_down) {
        (void)cardputer_enqueue_ascii(0x1BU);
    }
    s_esc_button_down = down;
}

static void cardputer_queue_matrix_press(uint64_t pressed_mask, cardputer_keycoord_t coord)
{
    uint8_t ascii = 0;

    if (cardputer_keymap_ascii_for_press(pressed_mask, coord, &ascii)) {
        (void)cardputer_enqueue_ascii(ascii);
    }
}

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
static const gpio_num_t s_original_select_gpios[3] = {
    (gpio_num_t)CARDPUTER_ORIGINAL_SELECT_PIN0,
    (gpio_num_t)CARDPUTER_ORIGINAL_SELECT_PIN1,
    (gpio_num_t)CARDPUTER_ORIGINAL_SELECT_PIN2,
};

static const gpio_num_t s_original_input_gpios[7] = {
    (gpio_num_t)13,
    (gpio_num_t)15,
    (gpio_num_t)3,
    (gpio_num_t)4,
    (gpio_num_t)5,
    (gpio_num_t)6,
    (gpio_num_t)7,
};

static void cardputer_original_select_line(uint8_t select_index)
{
    for (size_t bit = 0; bit < 3U; ++bit) {
        gpio_set_level(s_original_select_gpios[bit], (select_index >> bit) & 0x01U);
    }
    esp_rom_delay_us(3);
}

static uint64_t cardputer_original_scan_mask(void)
{
    uint64_t mask = 0;

    for (uint8_t select = 0; select < 8U; ++select) {
        cardputer_original_select_line(select);
        for (uint8_t input = 0; input < 7U; ++input) {
            cardputer_keycoord_t coord;

            if (gpio_get_level(s_original_input_gpios[input]) != 0) {
                continue;
            }
            if (!cardputer_keymap_decode_original(select, input, &coord)) {
                continue;
            }
            mask |= cardputer_keymap_mask_for_coord(coord);
        }
    }

    return mask;
}

static void cardputer_original_emit_presses(uint64_t new_presses, uint64_t current_mask)
{
    for (uint8_t index = 0; index < CARDPUTER_KEYMAP_KEYS; ++index) {
        cardputer_keycoord_t coord;

        if ((new_presses & (1ULL << index)) == 0U) {
            continue;
        }
        if (!cardputer_keymap_coord_from_index(index, &coord)) {
            continue;
        }
        cardputer_queue_matrix_press(current_mask, coord);
    }
}

static esp_err_t cardputer_original_keyboard_init(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << CARDPUTER_ORIGINAL_SELECT_PIN0) |
                        (1ULL << CARDPUTER_ORIGINAL_SELECT_PIN1) |
                        (1ULL << CARDPUTER_ORIGINAL_SELECT_PIN2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);

    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < 7U; ++i) {
        err = cardputer_configure_input(s_original_input_gpios[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_matrix_mask = cardputer_original_scan_mask();
    s_last_matrix_scan = xTaskGetTickCount();
    return ESP_OK;
}

static void cardputer_original_keyboard_poll(void)
{
    const TickType_t now = xTaskGetTickCount();
    uint64_t current_mask;
    uint64_t new_presses;

    if ((now - s_last_matrix_scan) < pdMS_TO_TICKS(CARDPUTER_MATRIX_SCAN_PERIOD_MS)) {
        return;
    }
    s_last_matrix_scan = now;

    current_mask = cardputer_original_scan_mask();
    new_presses = current_mask & ~s_matrix_mask;
    if (new_presses != 0U) {
        cardputer_original_emit_presses(new_presses, current_mask);
    }
    s_matrix_mask = current_mask;
}
#endif

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
static bool cardputer_coord_equal(cardputer_keycoord_t a, cardputer_keycoord_t b)
{
    return a.row == b.row && a.column == b.column;
}

static uint64_t cardputer_adv_fn_mask(void)
{
    return cardputer_keymap_mask_for_coord((cardputer_keycoord_t){
        .row = 2U,
        .column = 0U,
    });
}

static bool cardputer_adv_ctrl_active(uint64_t pressed_mask)
{
    return (pressed_mask &
            cardputer_keymap_mask_for_coord((cardputer_keycoord_t){ .row = 3U, .column = 0U })) != 0U;
}

static bool cardputer_adv_fn_effective(TickType_t now, uint64_t pressed_mask)
{
    if (cardputer_adv_ctrl_active(pressed_mask)) {
        return false;
    }
    if ((pressed_mask & cardputer_adv_fn_mask()) != 0U) {
        return true;
    }
    if (s_adv_fn_latch_until == 0) {
        return false;
    }
    return (int32_t)(now - s_adv_fn_latch_until) < 0;
}

static uint64_t cardputer_adv_effective_mask(TickType_t now,
                                             uint64_t pressed_mask,
                                             cardputer_keycoord_t coord)
{
    if (cardputer_keymap_has_fn_command(coord) && !cardputer_adv_fn_effective(now, pressed_mask)) {
        pressed_mask &= ~cardputer_adv_fn_mask();
    }
    return pressed_mask;
}

static void cardputer_adv_emit_press(TickType_t now, uint64_t pressed_mask, cardputer_keycoord_t coord)
{
    cardputer_queue_matrix_press(cardputer_adv_effective_mask(now, pressed_mask, coord), coord);
    if (cardputer_keymap_has_fn_command(coord) && (pressed_mask & cardputer_adv_fn_mask()) == 0U) {
        s_adv_fn_latch_until = 0;
    }
}

static void cardputer_adv_emit_pending(TickType_t now, uint64_t pressed_mask)
{
    if (!s_adv_pending_press.active) {
        return;
    }

    cardputer_adv_emit_press(now, pressed_mask, s_adv_pending_press.coord);
    s_adv_pending_press.active = false;
}

static void cardputer_adv_flush_expired_pending(TickType_t now)
{
    if (!s_adv_pending_press.active) {
        return;
    }
    if ((int32_t)(now - s_adv_pending_press.deadline) < 0) {
        return;
    }

    cardputer_adv_emit_pending(now, s_adv_pressed_mask);
}

static esp_err_t cardputer_adv_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = { reg, value };

    return i2c_master_write_to_device(CARDPUTER_ADV_I2C_PORT,
                                      CARDPUTER_ADV_ADDR,
                                      payload,
                                      sizeof(payload),
                                      pdMS_TO_TICKS(20));
}

static esp_err_t cardputer_adv_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(CARDPUTER_ADV_I2C_PORT,
                                        CARDPUTER_ADV_ADDR,
                                        &reg,
                                        1,
                                        value,
                                        1,
                                        pdMS_TO_TICKS(20));
}

static esp_err_t cardputer_adv_clear_interrupts(void)
{
    return cardputer_adv_write_reg(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_KEY_EVENT);
}

static esp_err_t cardputer_adv_flush(void)
{
    esp_err_t err;
    uint8_t count = 0;

    do {
        uint8_t remaining;

        err = cardputer_adv_read_reg(TCA8418_REG_KEY_LCK_EC, &count);
        if (err != ESP_OK) {
            return err;
        }
        count &= 0x0FU;
        remaining = count;
        while (remaining-- != 0U) {
            uint8_t event = 0;

            err = cardputer_adv_read_reg(TCA8418_REG_KEY_EVENT_A, &event);
            if (err != ESP_OK) {
                return err;
            }
        }
    } while (count != 0U);

    return cardputer_adv_clear_interrupts();
}

static esp_err_t cardputer_adv_keyboard_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CARDPUTER_ADV_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = CARDPUTER_ADV_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CARDPUTER_ADV_I2C_HZ,
        .clk_flags = 0,
    };
    esp_err_t err = i2c_param_config(CARDPUTER_ADV_I2C_PORT, &config);

    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(CARDPUTER_ADV_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = cardputer_configure_input((gpio_num_t)CARDPUTER_ADV_INT_GPIO);
    if (err != ESP_OK) {
        return err;
    }

    err = cardputer_adv_write_reg(TCA8418_REG_KP_GPIO_1, 0x7FU);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_KP_GPIO_2, 0xFFU);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_KP_GPIO_3, 0x00U);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_DEBOUNCE_DIS_1, 0x00U);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_DEBOUNCE_DIS_2, 0x00U);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_DEBOUNCE_DIS_3, 0x00U);
    if (err != ESP_OK) {
        return err;
    }
    err = cardputer_adv_write_reg(TCA8418_REG_CFG,
                                  TCA8418_CFG_AUTO_INCREMENT | TCA8418_CFG_KEY_INTERRUPT);
    if (err != ESP_OK) {
        return err;
    }

    s_last_adv_poll = xTaskGetTickCount();
    s_adv_pressed_mask = 0U;
    s_adv_fn_latch_until = 0;
    s_adv_pending_press.active = false;
    err = cardputer_adv_flush();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ADV keyboard ready");
    }
    return err;
}

static void cardputer_adv_handle_event(uint8_t event, uint64_t *pressed_mask, TickType_t now)
{
    cardputer_keycoord_t coord;
    bool pressed = false;
    uint64_t bit;

    if (!cardputer_keymap_decode_adv_event(event, &pressed, &coord)) {
        return;
    }

    bit = cardputer_keymap_mask_for_coord(coord);
    if (pressed) {
        if (s_adv_pending_press.active &&
            !cardputer_coord_equal(s_adv_pending_press.coord, coord) &&
            !(coord.row == 2U && coord.column == 0U)) {
            cardputer_adv_emit_pending(now, *pressed_mask);
        }

        *pressed_mask |= bit;
        if (cardputer_keymap_is_modifier(coord)) {
            if (coord.row == 2U && coord.column == 0U) {
                s_adv_fn_latch_until = now + pdMS_TO_TICKS(CARDPUTER_ADV_FN_CHORD_WINDOW_MS);
                if (s_adv_pending_press.active) {
                    cardputer_adv_emit_pending(now, *pressed_mask);
                }
            } else if (coord.row == 3U && coord.column <= 2U) {
                s_adv_fn_latch_until = 0;
                if (s_adv_pending_press.active) {
                    cardputer_adv_emit_pending(now, *pressed_mask);
                }
            }
            return;
        }

        if (cardputer_keymap_has_fn_command(coord) &&
            !cardputer_adv_fn_effective(now, *pressed_mask)) {
            s_adv_pending_press.active = true;
            s_adv_pending_press.coord = coord;
            s_adv_pending_press.deadline =
                now + pdMS_TO_TICKS(CARDPUTER_ADV_FN_CHORD_WINDOW_MS);
            return;
        }

        cardputer_adv_emit_press(now, *pressed_mask, coord);
    } else {
        if (s_adv_pending_press.active &&
            cardputer_coord_equal(s_adv_pending_press.coord, coord)) {
            cardputer_adv_emit_pending(now, *pressed_mask);
        }
        *pressed_mask &= ~bit;
    }
}

static void cardputer_adv_keyboard_poll(void)
{
    const TickType_t now = xTaskGetTickCount();
    uint8_t count = 0;

    cardputer_adv_flush_expired_pending(now);

    if (gpio_get_level((gpio_num_t)CARDPUTER_ADV_INT_GPIO) != 0 &&
        (now - s_last_adv_poll) < pdMS_TO_TICKS(CARDPUTER_MATRIX_SCAN_PERIOD_MS)) {
        return;
    }
    s_last_adv_poll = now;
    if (cardputer_adv_read_reg(TCA8418_REG_KEY_LCK_EC, &count) != ESP_OK) {
        return;
    }
    count &= 0x0FU;
    if (count != 0U) {
        while (count-- != 0U) {
            uint8_t event = 0;

            if (cardputer_adv_read_reg(TCA8418_REG_KEY_EVENT_A, &event) != ESP_OK) {
                break;
            }
            cardputer_adv_handle_event(event, &s_adv_pressed_mask, now);
        }
    }
    (void)cardputer_adv_clear_interrupts();
}
#endif

static void cardputer_console_task(void *arg)
{
    (void)arg;
    while (true) {
        const int ch = getchar();
        if (ch < 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const uint8_t ascii = cardputer_translate_input(ch);
        (void)xQueueSend(s_ascii_queue, &ascii, portMAX_DELAY);
    }
}

esp_err_t cardputer_keyboard_init(void)
{
    esp_err_t err = ESP_OK;

    if (s_ascii_queue != NULL) {
        return ESP_OK;
    }

    s_ascii_queue = xQueueCreate(CARDPUTER_UART_QUEUE_LENGTH, sizeof(uint8_t));
    if (s_ascii_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = cardputer_init_esc_button();
    if (err != ESP_OK) {
        vQueueDelete(s_ascii_queue);
        s_ascii_queue = NULL;
        return err;
    }

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
    err = cardputer_original_keyboard_init();
#elif CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
    err = cardputer_adv_keyboard_init();
#endif
    if (err != ESP_OK && !CONFIG_M5APPLE2_INPUT_USE_UART) {
        vQueueDelete(s_ascii_queue);
        s_ascii_queue = NULL;
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hardware keyboard init failed, keeping UART fallback: %s", esp_err_to_name(err));
    }

#if CONFIG_M5APPLE2_INPUT_USE_UART
    if (xTaskCreate(cardputer_console_task, "cardputer_console", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_ascii_queue);
        s_ascii_queue = NULL;
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
}

bool cardputer_keyboard_poll_ascii(uint8_t *ascii)
{
    if (s_ascii_queue == NULL) {
        return false;
    }

    cardputer_poll_esc_button();
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
    cardputer_original_keyboard_poll();
#elif CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
    cardputer_adv_keyboard_poll();
#endif
    return xQueueReceive(s_ascii_queue, ascii, 0) == pdTRUE;
}
