#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"
#include "cardputer/cardputer_display.h"
#include "cardputer/cardputer_keyboard.h"

static const char *TAG = "m5apple2";
static apple2_machine_t s_machine;
static cardputer_display_t s_display;
static uint8_t s_apple2_pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];

#ifdef M5APPLE2_HAS_APPLE2PLUS_ROM
extern const uint8_t apple2plus_rom_start[] asm("_binary_apple2plus_rom_start");
extern const uint8_t apple2plus_rom_end[] asm("_binary_apple2plus_rom_end");
#endif
#ifdef M5APPLE2_HAS_DISK2_ROM
extern const uint8_t disk2_rom_start[] asm("_binary_disk2_rom_start");
extern const uint8_t disk2_rom_end[] asm("_binary_disk2_rom_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_DO
extern const uint8_t dos_3_3_do_start[] asm("_binary_dos_3_3_do_start");
extern const uint8_t dos_3_3_do_end[] asm("_binary_dos_3_3_do_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_PO
extern const uint8_t dos_3_3_po_start[] asm("_binary_dos_3_3_po_start");
extern const uint8_t dos_3_3_po_end[] asm("_binary_dos_3_3_po_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_DSK
extern const uint8_t dos_3_3_dsk_start[] asm("_binary_dos_3_3_dsk_start");
extern const uint8_t dos_3_3_dsk_end[] asm("_binary_dos_3_3_dsk_end");
#endif

#define APP_DSK_PROBE_INSTRUCTIONS 900000U

typedef enum {
    APP_DISK_ORDER_DOS33 = 0,
    APP_DISK_ORDER_PRODOS = 1,
} app_disk_order_t;

#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
static unsigned app_count_nonzero_range(const apple2_machine_t *machine, uint16_t base, uint16_t size)
{
    unsigned nonzero = 0;

    for (uint16_t i = 0; i < size; ++i) {
        if (machine->memory[(uint16_t)(base + i)] != 0U) {
            nonzero++;
        }
    }

    return nonzero;
}

static int app_score_dsk_order(const uint8_t *image, size_t image_size, app_disk_order_t order)
{
    apple2_machine_t probe;
    bool entered_stage2 = false;
    bool loaded = false;

    apple2_machine_init(&probe, &(apple2_config_t){ .cpu_hz = CONFIG_M5APPLE2_CPU_HZ });
    if (!apple2_machine_load_system_rom(&probe,
                                        apple2plus_rom_start,
                                        (size_t)(apple2plus_rom_end - apple2plus_rom_start)) ||
        !apple2_machine_load_slot6_rom(&probe,
                                       disk2_rom_start,
                                       (size_t)(disk2_rom_end - disk2_rom_start))) {
        return INT_MIN / 2;
    }

    loaded = (order == APP_DISK_ORDER_PRODOS)
                 ? apple2_machine_load_drive0_po(&probe, image, image_size)
                 : apple2_machine_load_drive0_dsk(&probe, image, image_size);
    if (!loaded) {
        return INT_MIN / 2;
    }

    for (uint32_t i = 0; i < APP_DSK_PROBE_INSTRUCTIONS; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&probe);

        if (cpu.pc >= 0x3700U && cpu.pc < 0x4000U) {
            entered_stage2 = true;
        }
        if (entered_stage2) {
            if (cpu.pc < 0x0100U) {
                break;
            }
            if (cpu.pc >= 0xFD00U && probe.disk2.nibble_pos[0] <= 384U) {
                break;
            }
        }
        apple2_machine_step_instruction(&probe);
    }

    {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&probe);
        int score = 0;

        score += (int)app_count_nonzero_range(&probe, 0x1D00U, 0x0400U);
        score += (int)app_count_nonzero_range(&probe, 0x2A00U, 0x0100U);
        if (probe.disk2.nibble_pos[0] > 384U) {
            score += 512;
        }
        if (probe.disk2.quarter_track[0] != 0U) {
            score += 128;
        }
        if (cpu.pc >= 0x3000U && cpu.pc < 0x4000U) {
            score += 128;
        }
        if (entered_stage2) {
            score += 64;
        }
        if (cpu.pc < 0x0100U) {
            score -= 512;
        }
        if (cpu.pc >= 0xFD00U) {
            score -= 256;
        }
        return score;
    }
}

static app_disk_order_t app_probe_dsk_order(const uint8_t *image, size_t image_size)
{
    const int dos_score = app_score_dsk_order(image, image_size, APP_DISK_ORDER_DOS33);
    const int prodos_score = app_score_dsk_order(image, image_size, APP_DISK_ORDER_PRODOS);

    ESP_LOGI(TAG, "Probed .dsk order: dos=%d prodos=%d", dos_score, prodos_score);
    return (prodos_score > dos_score) ? APP_DISK_ORDER_PRODOS : APP_DISK_ORDER_DOS33;
}
#endif

static void app_puts_at(uint8_t row, uint8_t column, const char *text)
{
    uint16_t address = apple2_text_row_address(false, row);
    address = (uint16_t)(address + column);

    while (*text != '\0' && column < 40U) {
        char ch = *text++;
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch < 32 || ch > 95) {
            ch = ' ';
        }
        s_machine.memory[address++] = (uint8_t)(ch | 0x80U);
        column++;
    }
}

static void app_show_status_screen(const char *line1, const char *line2, const char *line3)
{
    for (uint8_t row = 0; row < 24U; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);
        for (uint8_t column = 0; column < 40U; ++column) {
            s_machine.memory[(uint16_t)(base + column)] = (uint8_t)(' ' | 0x80U);
        }
    }

    app_puts_at(2, 12, "M5APPLE2");
    if (line1 != NULL) {
        app_puts_at(8, 2, line1);
    }
    if (line2 != NULL) {
        app_puts_at(10, 2, line2);
    }
    if (line3 != NULL) {
        app_puts_at(12, 2, line3);
    }
    app_puts_at(20, 2, "ESC RESETS THE EMULATOR");
}

static bool app_load_system_rom(void)
{
#ifdef M5APPLE2_HAS_APPLE2PLUS_ROM
    const size_t rom_size = (size_t)(apple2plus_rom_end - apple2plus_rom_start);
    if (!apple2_machine_load_system_rom(&s_machine, apple2plus_rom_start, rom_size)) {
        ESP_LOGE(TAG, "Embedded ROM rejected, size=%u", (unsigned)rom_size);
        return false;
    }
    ESP_LOGI(TAG, "Loaded embedded Apple II ROM (%u bytes)", (unsigned)rom_size);
    return true;
#else
    return false;
#endif
}

static bool app_load_slot6_rom(void)
{
#ifdef M5APPLE2_HAS_DISK2_ROM
    const size_t rom_size = (size_t)(disk2_rom_end - disk2_rom_start);
    if (!apple2_machine_load_slot6_rom(&s_machine, disk2_rom_start, rom_size)) {
        ESP_LOGE(TAG, "Embedded Disk II ROM rejected, size=%u", (unsigned)rom_size);
        return false;
    }
    ESP_LOGI(TAG, "Loaded embedded Disk II ROM (%u bytes)", (unsigned)rom_size);
    return true;
#else
    return false;
#endif
}

static bool app_load_drive0_image(void)
{
#ifdef M5APPLE2_HAS_DOS33_DO
    {
        const size_t image_size = (size_t)(dos_3_3_do_end - dos_3_3_do_start);
        if (!apple2_machine_load_drive0_do(&s_machine, dos_3_3_do_start, image_size)) {
            ESP_LOGE(TAG, "Embedded DOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded DOS-order disk (%u bytes)", (unsigned)image_size);
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_PO
    {
        const size_t image_size = (size_t)(dos_3_3_po_end - dos_3_3_po_start);
        if (!apple2_machine_load_drive0_po(&s_machine, dos_3_3_po_start, image_size)) {
            ESP_LOGE(TAG, "Embedded ProDOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded ProDOS-order disk (%u bytes)", (unsigned)image_size);
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_DSK
    {
        const size_t image_size = (size_t)(dos_3_3_dsk_end - dos_3_3_dsk_start);
        bool loaded = false;

#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
        const app_disk_order_t order = app_probe_dsk_order(dos_3_3_dsk_start, image_size);
        loaded = (order == APP_DISK_ORDER_PRODOS)
                     ? apple2_machine_load_drive0_po(&s_machine, dos_3_3_dsk_start, image_size)
                     : apple2_machine_load_drive0_dsk(&s_machine, dos_3_3_dsk_start, image_size);
        if (loaded) {
            ESP_LOGI(TAG,
                     "Loaded embedded .dsk disk (%u bytes, %s order)",
                     (unsigned)image_size,
                     (order == APP_DISK_ORDER_PRODOS) ? "ProDOS" : "DOS");
        }
#else
        loaded = apple2_machine_load_drive0_dsk(&s_machine, dos_3_3_dsk_start, image_size);
        if (loaded) {
            ESP_LOGI(TAG, "Loaded embedded .dsk disk (%u bytes, DOS fallback)", (unsigned)image_size);
        }
#endif
        if (!loaded) {
            ESP_LOGE(TAG, "Embedded .dsk disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        return true;
    }
#else
    return false;
#endif
}

void app_main(void)
{
    apple2_config_t apple2_config = {
        .cpu_hz = CONFIG_M5APPLE2_CPU_HZ,
    };
    bool rom_loaded = false;
    bool slot6_loaded = false;
    bool drive0_loaded = false;
    int64_t last_cpu_tick_us;
    int64_t last_frame_tick_us;

    apple2_machine_init(&s_machine, &apple2_config);

    ESP_ERROR_CHECK(cardputer_display_init(&s_display));
    ESP_ERROR_CHECK(cardputer_keyboard_init());
    ESP_LOGI(TAG, "Cardputer display ready: %" PRIu16 "x%" PRIu16,
             s_display.native_width, s_display.native_height);

    rom_loaded = app_load_system_rom();
    slot6_loaded = app_load_slot6_rom();
    drive0_loaded = app_load_drive0_image();
    if (rom_loaded && !slot6_loaded) {
        ESP_LOGW(TAG, "System ROM loaded without a separate Disk II ROM");
    }
    if (drive0_loaded && !rom_loaded) {
        ESP_LOGW(TAG, "Disk image loaded without a system ROM");
    }
    if (drive0_loaded && !slot6_loaded && !s_machine.slot6_rom_loaded) {
        ESP_LOGW(TAG, "Disk image loaded without Disk II ROM support");
    }
    if (!rom_loaded) {
        app_show_status_screen("PLACE APPLE2PLUS.ROM IN", "roms/ AND REBUILD.", "RUNS WITHOUT ROM SHOW STATUS.");
    }

    last_cpu_tick_us = esp_timer_get_time();
    last_frame_tick_us = last_cpu_tick_us;

    while (true) {
        uint8_t ascii = 0;
        if (cardputer_keyboard_poll_ascii(&ascii)) {
            if (ascii == 0x1BU) {
                apple2_machine_reset(&s_machine);
                if (!rom_loaded) {
                    app_show_status_screen("PLACE APPLE2PLUS.ROM IN", "roms/ AND REBUILD.", "RUNS WITHOUT ROM SHOW STATUS.");
                }
            } else {
                apple2_machine_set_key(&s_machine, ascii);
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (rom_loaded) {
            uint64_t budget = (uint64_t)(now_us - last_cpu_tick_us) * apple2_config.cpu_hz / 1000000ULL;
            if (budget > (uint64_t)apple2_config.cpu_hz / 10ULL) {
                budget = (uint64_t)apple2_config.cpu_hz / 10ULL;
            }
            if (budget > 0U) {
                apple2_machine_step(&s_machine, (uint32_t)budget);
            }
        }
        last_cpu_tick_us = now_us;

        if ((now_us - last_frame_tick_us) >= 16666) {
            apple2_machine_render(&s_machine, s_apple2_pixels);
            ESP_ERROR_CHECK(cardputer_display_present_apple2(&s_display,
                                                             s_apple2_pixels,
                                                             APPLE2_VIDEO_WIDTH,
                                                             APPLE2_VIDEO_HEIGHT));
            last_frame_tick_us = now_us;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
