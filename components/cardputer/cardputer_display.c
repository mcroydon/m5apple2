#include "cardputer/cardputer_display.h"

#include "apple2/apple2_video.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

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

static spi_host_device_t cardputer_spi_host(void)
{
    return CONFIG_M5APPLE2_LCD_HOST_ID == 3 ? SPI3_HOST : SPI2_HOST;
}

esp_err_t cardputer_display_init(cardputer_display_t *display)
{
    static bool s_spi_initialized;

    display->native_width = CONFIG_M5APPLE2_LCD_WIDTH;
    display->native_height = CONFIG_M5APPLE2_LCD_HEIGHT;

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
        .trans_queue_depth = 10,
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

esp_err_t cardputer_display_present_apple2(cardputer_display_t *display,
                                           const uint8_t *pixels,
                                           uint16_t width,
                                           uint16_t height)
{
    static uint16_t s_framebuffer[CONFIG_M5APPLE2_LCD_WIDTH * CONFIG_M5APPLE2_LCD_HEIGHT];
    const uint16_t dst_width = display->native_width;
    const uint16_t dst_height = display->native_height;
    uint16_t out_width = dst_width;
    uint16_t out_height = dst_height;
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;

    if (M5APPLE2_LCD_PRESERVE_ASPECT_VALUE) {
        out_width = dst_width;
        out_height = (uint16_t)((uint32_t)dst_width * height / width);
        if (out_height > dst_height) {
            out_height = dst_height;
            out_width = (uint16_t)((uint32_t)dst_height * width / height);
        }
        offset_x = (uint16_t)((dst_width - out_width) / 2U);
        offset_y = (uint16_t)((dst_height - out_height) / 2U);
    }

    for (uint32_t i = 0; i < (uint32_t)dst_width * dst_height; ++i) {
        s_framebuffer[i] = apple2_palette_rgb565(APPLE2_COLOR_BLACK);
    }

    for (uint16_t y = 0; y < out_height; ++y) {
        const uint16_t src_y = (uint16_t)((uint32_t)y * height / out_height);
        for (uint16_t x = 0; x < out_width; ++x) {
            const uint16_t src_x = (uint16_t)((uint32_t)x * width / out_width);
            const uint16_t dst_index = (uint16_t)((y + offset_y) * dst_width + x + offset_x);
            const uint8_t color = pixels[(size_t)src_y * width + src_x];
            s_framebuffer[dst_index] = apple2_palette_rgb565(color);
        }
    }

    return esp_lcd_panel_draw_bitmap(display->panel, 0, 0, dst_width, dst_height, s_framebuffer);
}
