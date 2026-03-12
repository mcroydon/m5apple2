#include "cardputer/cardputer_keyboard.h"

#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t s_ascii_queue;

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
#if !CONFIG_M5APPLE2_INPUT_USE_UART
        return ESP_OK;
#endif
    if (s_ascii_queue != NULL) {
        return ESP_OK;
    }

    s_ascii_queue = xQueueCreate(32, sizeof(uint8_t));
    if (s_ascii_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(cardputer_console_task, "cardputer_console", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_ascii_queue);
        s_ascii_queue = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool cardputer_keyboard_poll_ascii(uint8_t *ascii)
{
    if (s_ascii_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_ascii_queue, ascii, 0) == pdTRUE;
}
