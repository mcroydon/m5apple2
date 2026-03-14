#include "cardputer/cardputer_display.h"

#include "apple2/apple2_video.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#ifdef CONFIG_M5APPLE2_LCD_SWAP_XY
#define M5APPLE2_LCD_SWAP_XY_VALUE true
#else
#define M5APPLE2_LCD_SWAP_XY_VALUE false
#endif

#ifdef CONFIG_M5APPLE2_LCD_MIRROR_X
#define M5APPLE2_LCD_MIRROR_X_VALUE true
#else
#define M5APPLE2_LCD_MIRROR_X_VALUE false
#endif

#ifdef CONFIG_M5APPLE2_LCD_MIRROR_Y
#define M5APPLE2_LCD_MIRROR_Y_VALUE true
#else
#define M5APPLE2_LCD_MIRROR_Y_VALUE false
#endif

#ifdef CONFIG_M5APPLE2_LCD_PRESERVE_ASPECT
#define M5APPLE2_LCD_PRESERVE_ASPECT_VALUE true
#else
#define M5APPLE2_LCD_PRESERVE_ASPECT_VALUE false
#endif

#ifdef CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
#define M5APPLE2_LCD_TRANS_QUEUE_DEPTH 1
#else
#define M5APPLE2_LCD_TRANS_QUEUE_DEPTH 10
#endif

#ifndef CONFIG_M5APPLE2_LCD_MARGIN_LEFT
#ifdef CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
#define CONFIG_M5APPLE2_LCD_MARGIN_LEFT 4
#else
#define CONFIG_M5APPLE2_LCD_MARGIN_LEFT 0
#endif
#endif

#ifndef CONFIG_M5APPLE2_LCD_MARGIN_RIGHT
#define CONFIG_M5APPLE2_LCD_MARGIN_RIGHT 0
#endif

#ifndef CONFIG_M5APPLE2_LCD_MARGIN_TOP
#define CONFIG_M5APPLE2_LCD_MARGIN_TOP 0
#endif

#ifndef CONFIG_M5APPLE2_LCD_MARGIN_BOTTOM
#ifdef CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
#define CONFIG_M5APPLE2_LCD_MARGIN_BOTTOM 4
#else
#define CONFIG_M5APPLE2_LCD_MARGIN_BOTTOM 0
#endif
#endif

static spi_host_device_t cardputer_spi_host(void)
{
    return CONFIG_M5APPLE2_LCD_HOST_ID == 3 ? SPI3_HOST : SPI2_HOST;
}

static const char *TAG = "cardputer_display";
static uint16_t s_framebuffer[CONFIG_M5APPLE2_LCD_WIDTH * CONFIG_M5APPLE2_LCD_HEIGHT];

static void cardputer_display_safe_area(uint16_t panel_width,
                                        uint16_t panel_height,
                                        uint16_t *origin_x,
                                        uint16_t *origin_y,
                                        uint16_t *safe_width,
                                        uint16_t *safe_height)
{
    const uint16_t margin_left = CONFIG_M5APPLE2_LCD_MARGIN_LEFT;
    const uint16_t margin_right = CONFIG_M5APPLE2_LCD_MARGIN_RIGHT;
    const uint16_t margin_top = CONFIG_M5APPLE2_LCD_MARGIN_TOP;
    const uint16_t margin_bottom = CONFIG_M5APPLE2_LCD_MARGIN_BOTTOM;
    const uint16_t clamped_x = (margin_left < panel_width) ? margin_left : panel_width;
    const uint16_t clamped_y = (margin_top < panel_height) ? margin_top : panel_height;
    const uint16_t remaining_width = (clamped_x < panel_width) ? (uint16_t)(panel_width - clamped_x) : 0U;
    const uint16_t remaining_height = (clamped_y < panel_height) ? (uint16_t)(panel_height - clamped_y) : 0U;
    const uint16_t clamped_width =
        (margin_right < remaining_width) ? (uint16_t)(remaining_width - margin_right) : 1U;
    const uint16_t clamped_height =
        (margin_bottom < remaining_height) ? (uint16_t)(remaining_height - margin_bottom) : 1U;

    *origin_x = clamped_x;
    *origin_y = clamped_y;
    *safe_width = clamped_width;
    *safe_height = clamped_height;
}

static void cardputer_display_clear(uint16_t color)
{
    if (color == 0U) {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        return;
    }

    for (uint32_t i = 0; i < (uint32_t)CONFIG_M5APPLE2_LCD_WIDTH * CONFIG_M5APPLE2_LCD_HEIGHT; ++i) {
        s_framebuffer[i] = color;
    }
}

static void cardputer_display_prepare_graphics_map(cardputer_display_t *display,
                                                   uint16_t src_width,
                                                   uint16_t src_height,
                                                   uint16_t offset_x,
                                                   uint16_t offset_y,
                                                   uint16_t out_width,
                                                   uint16_t out_height)
{
    if (display->graphics_map_valid &&
        display->graphics_src_width == src_width &&
        display->graphics_src_height == src_height &&
        display->graphics_offset_x == offset_x &&
        display->graphics_offset_y == offset_y &&
        display->graphics_out_width == out_width &&
        display->graphics_out_height == out_height) {
        return;
    }

    display->graphics_src_width = src_width;
    display->graphics_src_height = src_height;
    display->graphics_offset_x = offset_x;
    display->graphics_offset_y = offset_y;
    display->graphics_out_width = out_width;
    display->graphics_out_height = out_height;

    for (uint16_t x = 0; x < out_width; ++x) {
        display->graphics_src_x[x] = (uint16_t)((uint32_t)x * src_width / out_width);
    }
    for (uint16_t y = 0; y < out_height; ++y) {
        display->graphics_src_y[y] = (uint16_t)((uint32_t)y * src_height / out_height);
    }

    display->graphics_map_valid = true;
}

static bool cardputer_display_glyph_group_on(const uint8_t *glyph,
                                             uint8_t src_x0,
                                             uint8_t src_x1,
                                             uint8_t src_y0,
                                             uint8_t src_y1)
{
    for (uint8_t sy = src_y0; sy <= src_y1; ++sy) {
        const uint8_t row_bits = glyph[sy];
        for (uint8_t sx = src_x0; sx <= src_x1; ++sx) {
            if ((row_bits & (uint8_t)(1U << (6U - sx))) != 0U) {
                return true;
            }
        }
    }
    return false;
}

static bool cardputer_display_text_row_active(const uint8_t *memory,
                                              const apple2_video_state_t *state,
                                              uint8_t row)
{
    const uint16_t row_address = apple2_text_row_address(state->page2, row);

    for (uint8_t column = 0; column < 40U; ++column) {
        bool inverse = false;
        const uint8_t code = memory[(uint16_t)(row_address + column)];
        const bool flashing = apple2_text_code_is_flashing(code) && state->flash_state;
        const uint8_t ascii = apple2_text_code_to_ascii(code, &inverse);

        if (ascii != ' ' || inverse || flashing) {
            return true;
        }
    }

    return false;
}

static uint8_t cardputer_display_text_row_focus_score(const uint8_t *memory,
                                                      const apple2_video_state_t *state,
                                                      uint8_t row)
{
    const uint16_t row_address = apple2_text_row_address(state->page2, row);
    uint8_t score = 0U;

    for (uint8_t column = 0; column < 40U; ++column) {
        const uint8_t code = memory[(uint16_t)(row_address + column)];

        if (apple2_text_code_is_flash_space(code)) {
            return 2U;
        }
        if (apple2_text_code_is_flashing(code)) {
            score = 1U;
        }
    }

    return score;
}

esp_err_t cardputer_display_init(cardputer_display_t *display)
{
    static bool s_spi_initialized;

    memset(display, 0, sizeof(*display));
    display->native_width = CONFIG_M5APPLE2_LCD_WIDTH;
    display->native_height = CONFIG_M5APPLE2_LCD_HEIGHT;
    ESP_LOGI(TAG,
             "LCD cfg host=%d cs=%d dc=%d rst=%d bklt=%d gap=%d,%d swap=%d mirror=%d,%d aspect=%d margin=%d,%d,%d,%d",
             CONFIG_M5APPLE2_LCD_HOST_ID,
             CONFIG_M5APPLE2_LCD_PIN_CS,
             CONFIG_M5APPLE2_LCD_PIN_DC,
             CONFIG_M5APPLE2_LCD_PIN_RST,
             CONFIG_M5APPLE2_LCD_PIN_BKLT,
             CONFIG_M5APPLE2_LCD_OFFSET_X,
             CONFIG_M5APPLE2_LCD_OFFSET_Y,
             M5APPLE2_LCD_SWAP_XY_VALUE ? 1 : 0,
             M5APPLE2_LCD_MIRROR_X_VALUE ? 1 : 0,
             M5APPLE2_LCD_MIRROR_Y_VALUE ? 1 : 0,
             M5APPLE2_LCD_PRESERVE_ASPECT_VALUE ? 1 : 0,
             CONFIG_M5APPLE2_LCD_MARGIN_LEFT,
             CONFIG_M5APPLE2_LCD_MARGIN_RIGHT,
             CONFIG_M5APPLE2_LCD_MARGIN_TOP,
             CONFIG_M5APPLE2_LCD_MARGIN_BOTTOM);

    if (!s_spi_initialized) {
        const spi_bus_config_t bus_config = {
            .sclk_io_num = CONFIG_M5APPLE2_LCD_PIN_CLK,
            .mosi_io_num = CONFIG_M5APPLE2_LCD_PIN_MOSI,
            .miso_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = CONFIG_M5APPLE2_LCD_WIDTH * CONFIG_M5APPLE2_LCD_HEIGHT * sizeof(uint16_t),
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(cardputer_spi_host(), &bus_config, SPI_DMA_CH_AUTO),
                            "cardputer_display", "spi bus init failed");
        s_spi_initialized = true;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_M5APPLE2_LCD_PIN_DC,
        .cs_gpio_num = CONFIG_M5APPLE2_LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .spi_mode = 0,
        /* The original Cardputer uses a single persistent framebuffer. Keep the LCD queue shallow
         * there so we do not render into memory that SPI DMA is still transmitting. */
        .trans_queue_depth = M5APPLE2_LCD_TRANS_QUEUE_DEPTH,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cardputer_spi_host(),
                                                 &io_config,
                                                 &display->io),
                        "cardputer_display", "panel io init failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_M5APPLE2_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(display->io, &panel_config, &display->panel),
                        "cardputer_display", "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(display->panel), "cardputer_display", "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(display->panel), "cardputer_display", "panel start failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(display->panel, true),
                        "cardputer_display", "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(display->panel, M5APPLE2_LCD_SWAP_XY_VALUE),
                        "cardputer_display", "panel swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(display->panel,
                                             M5APPLE2_LCD_MIRROR_X_VALUE,
                                             M5APPLE2_LCD_MIRROR_Y_VALUE),
                        "cardputer_display", "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(display->panel,
                                              CONFIG_M5APPLE2_LCD_OFFSET_X,
                                              CONFIG_M5APPLE2_LCD_OFFSET_Y),
                        "cardputer_display", "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(display->panel, true),
                        "cardputer_display", "panel enable failed");

    if (CONFIG_M5APPLE2_LCD_PIN_BKLT >= 0) {
        const gpio_config_t backlight_gpio = {
            .pin_bit_mask = 1ULL << CONFIG_M5APPLE2_LCD_PIN_BKLT,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&backlight_gpio), "cardputer_display", "backlight gpio failed");
        gpio_set_level(CONFIG_M5APPLE2_LCD_PIN_BKLT, 1);
    }

    return ESP_OK;
}

esp_err_t cardputer_display_present_apple2_text40(cardputer_display_t *display,
                                                  const uint8_t *memory,
                                                  const apple2_video_state_t *state)
{
    const uint16_t dst_width = display->native_width;
    const uint16_t dst_height = display->native_height;
    uint16_t safe_x = 0;
    uint16_t safe_y = 0;
    uint16_t safe_width = dst_width;
    uint16_t safe_height = dst_height;
    const uint16_t fg = apple2_palette_rgb565(APPLE2_COLOR_WHITE);
    const uint16_t bg = apple2_palette_rgb565(APPLE2_COLOR_BLACK);
    uint8_t first_active = 24U;
    uint8_t last_active = 0U;
    uint8_t focus_row = 24U;
    uint8_t focus_score = 0U;
    uint8_t first_row;
    uint8_t last_row;
    uint8_t active_rows;
    uint8_t visible_rows;
    uint8_t cell_height;
    uint16_t origin_y;

    cardputer_display_safe_area(dst_width, dst_height, &safe_x, &safe_y, &safe_width, &safe_height);
    cardputer_display_clear(bg);

    for (uint8_t row = 0; row < 24U; ++row) {
        if (!cardputer_display_text_row_active(memory, state, row)) {
            continue;
        }
        if (first_active == 24U) {
            first_active = row;
        }
        last_active = row;
        {
            const uint8_t row_focus_score = cardputer_display_text_row_focus_score(memory, state, row);

            if (row_focus_score >= focus_score) {
                focus_score = row_focus_score;
                focus_row = row;
            }
        }
    }

    if (first_active == 24U) {
        first_active = 0U;
        last_active = 0U;
    }
    if (focus_row == 24U) {
        focus_row = last_active;
    }

    active_rows = (uint8_t)(last_active - first_active + 1U);
    if (active_rows <= 16U) {
        visible_rows = active_rows;
        if (visible_rows < 12U) {
            visible_rows = 12U;
        }
        cell_height = 8U;
    } else if (active_rows <= 19U) {
        visible_rows = active_rows;
        if (visible_rows < 17U) {
            visible_rows = 17U;
        }
        cell_height = 7U;
    } else {
        visible_rows = 22U;
        cell_height = 6U;
    }
    if ((uint16_t)visible_rows * cell_height > safe_height) {
        visible_rows = (uint8_t)(safe_height / cell_height);
        if (visible_rows == 0U) {
            visible_rows = 1U;
        }
    }

    if (focus_row + 1U > visible_rows) {
        first_row = (uint8_t)(focus_row + 1U - visible_rows);
    } else {
        first_row = 0U;
    }
    if (first_row > (uint8_t)(24U - visible_rows)) {
        first_row = (uint8_t)(24U - visible_rows);
    }
    if (active_rows <= visible_rows && first_active < first_row) {
        first_row = first_active;
    }
    last_row = (uint8_t)(first_row + visible_rows - 1U);
    origin_y = (uint16_t)(safe_y + ((safe_height - visible_rows * cell_height) / 2U));

    for (uint8_t row = first_row; row <= last_row; ++row) {
        const uint16_t row_address = apple2_text_row_address(state->page2, row);
        const uint16_t dst_y = (uint16_t)(origin_y + (row - first_row) * cell_height);

        for (uint8_t column = 0; column < 40U; ++column) {
            bool inverse = false;
            const uint8_t code = memory[(uint16_t)(row_address + column)];
            const bool flashing = apple2_text_code_is_flashing(code);
            const uint8_t ascii = apple2_text_code_to_ascii(code, &inverse);
            const bool cell_inverse = inverse || (flashing && state->flash_state);
            const uint16_t cell_bg = cell_inverse ? fg : bg;
            const uint16_t cell_fg = cell_inverse ? bg : fg;
            const uint16_t dst_x0 = (uint16_t)(safe_x + ((uint32_t)column * safe_width) / 40U);
            const uint16_t dst_x1 = (uint16_t)(safe_x + ((uint32_t)(column + 1U) * safe_width) / 40U);
            const uint16_t cell_width = (dst_x1 > dst_x0) ? (uint16_t)(dst_x1 - dst_x0) : 1U;
            const uint8_t *glyph = apple2_ascii_font(ascii);

            for (uint8_t ty = 0; ty < cell_height; ++ty) {
                const uint32_t base = (uint32_t)(dst_y + ty) * dst_width + dst_x0;
                for (uint16_t tx = 0; tx < cell_width; ++tx) {
                    s_framebuffer[base + tx] = cell_bg;
                }
                for (uint16_t tx = 0; tx < cell_width; ++tx) {
                    const uint8_t src_x0 = (uint8_t)(((uint32_t)tx * 7U) / cell_width);
                    const uint8_t src_x1 =
                        (uint8_t)(((((uint32_t)tx + 1U) * 7U) - 1U) / cell_width);
                    const uint8_t src_y0 = (uint8_t)((ty * 8U) / cell_height);
                    const uint8_t src_y1 = (uint8_t)((((ty + 1U) * 8U) - 1U) / cell_height);

                    if (cardputer_display_glyph_group_on(glyph,
                                                         src_x0,
                                                         src_x1,
                                                         src_y0,
                                                         src_y1)) {
                        s_framebuffer[base + tx] = cell_fg;
                    }
                }
            }
        }
    }

    return esp_lcd_panel_draw_bitmap(display->panel, 0, 0, dst_width, dst_height, s_framebuffer);
}

esp_err_t cardputer_display_present_apple2(cardputer_display_t *display,
                                           const uint8_t *pixels,
                                           uint16_t width,
                                           uint16_t height)
{
    const uint16_t dst_width = display->native_width;
    const uint16_t dst_height = display->native_height;
    uint16_t safe_x = 0;
    uint16_t safe_y = 0;
    uint16_t safe_width = dst_width;
    uint16_t safe_height = dst_height;
    uint16_t out_width;
    uint16_t out_height;
    uint16_t offset_x;
    uint16_t offset_y;

    cardputer_display_safe_area(dst_width, dst_height, &safe_x, &safe_y, &safe_width, &safe_height);
    out_width = safe_width;
    out_height = safe_height;
    offset_x = safe_x;
    offset_y = safe_y;

    if (M5APPLE2_LCD_PRESERVE_ASPECT_VALUE) {
        out_width = safe_width;
        out_height = (uint16_t)((uint32_t)safe_width * height / width);
        if (out_height > safe_height) {
            out_height = safe_height;
            out_width = (uint16_t)((uint32_t)safe_height * width / height);
        }
        offset_x = (uint16_t)(safe_x + ((safe_width - out_width) / 2U));
        offset_y = (uint16_t)(safe_y + ((safe_height - out_height) / 2U));
    }

    cardputer_display_prepare_graphics_map(display, width, height, offset_x, offset_y, out_width, out_height);
    if (offset_x != 0U || offset_y != 0U || out_width != dst_width || out_height != dst_height) {
        cardputer_display_clear(apple2_palette_rgb565(APPLE2_COLOR_BLACK));
    }

    for (uint16_t y = 0; y < out_height; ++y) {
        const uint16_t src_y = display->graphics_src_y[y];
        uint16_t *dst_row = &s_framebuffer[(uint32_t)(y + offset_y) * dst_width + offset_x];
        const uint8_t *src_row = &pixels[(size_t)src_y * width];

        for (uint16_t x = 0; x < out_width; ++x) {
            dst_row[x] = apple2_palette_rgb565(src_row[display->graphics_src_x[x]]);
        }
    }

    return esp_lcd_panel_draw_bitmap(display->panel, 0, 0, dst_width, dst_height, s_framebuffer);
}
