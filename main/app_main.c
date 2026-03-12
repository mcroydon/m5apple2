#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apple2/apple2_machine.h"
#include "cardputer/cardputer_display.h"
#include "cardputer/cardputer_keyboard.h"

static const char *TAG = "m5apple2";

void app_main(void)
{
    apple2_machine_t machine;
    apple2_config_t apple2_config = {
        .cpu_hz = CONFIG_M5APPLE2_CPU_HZ,
    };
    cardputer_display_t display;

    apple2_machine_init(&machine, &apple2_config);

    ESP_ERROR_CHECK(cardputer_display_init(&display));
    ESP_LOGI(TAG, "Cardputer display ready: %" PRIu16 "x%" PRIu16,
             display.native_width, display.native_height);

    while (true) {
        uint8_t ascii = 0;
        if (cardputer_keyboard_poll_ascii(&ascii)) {
            apple2_machine_set_key(&machine, ascii);
            ESP_LOGI(TAG, "Queued key 0x%02x", ascii);
        }

        apple2_machine_step(&machine, apple2_config.cpu_hz / 60U);

        const char *status = apple2_machine_status(&machine);
        ESP_LOGI(TAG, "%s", status);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

