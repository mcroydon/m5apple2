#include "cardputer/cardputer_audio.h"

#ifdef ESP_PLATFORM

#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
#include "driver/i2c.h"
#endif

#ifndef CONFIG_M5APPLE2_AUDIO_SAMPLE_RATE
#define CONFIG_M5APPLE2_AUDIO_SAMPLE_RATE 22050
#endif
#ifndef CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES
#define CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES 512
#endif

#define AUDIO_TOGGLE_RING_SIZE 64U
#define AUDIO_LEVEL_HIGH ((int16_t)8000)
#define AUDIO_LEVEL_LOW  ((int16_t)-8000)

/*
 * I2S pin assignments for M5Stack Cardputer.
 * Both Original and ADV variants use the same GPIO mapping.
 */
#define AUDIO_I2S_BCK_GPIO  41
#define AUDIO_I2S_WS_GPIO   43
#define AUDIO_I2S_DOUT_GPIO 42

/*
 * ES8311 codec configuration (Original variant only).
 * The ES8311 requires I2C register setup before I2S data flows.
 */
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
#define ES8311_I2C_PORT    I2C_NUM_0
#define ES8311_I2C_ADDR    0x18U
#define ES8311_I2C_HZ      100000U
#define ES8311_I2C_SDA_GPIO 8
#define ES8311_I2C_SCL_GPIO 9

/* ES8311 register addresses */
#define ES8311_REG_RESET        0x00U
#define ES8311_REG_CLK_MANAGER1 0x01U
#define ES8311_REG_CLK_MANAGER2 0x02U
#define ES8311_REG_SYSTEM       0x0DU
#define ES8311_REG_SYSTEM2      0x12U
#define ES8311_REG_SYSTEM3      0x13U
#define ES8311_REG_DAC1         0x32U
#define ES8311_REG_DAC2         0x37U
#endif

static const char *TAG = "cardputer_audio";

struct cardputer_audio {
    uint64_t toggle_ring[AUDIO_TOGGLE_RING_SIZE];
    uint8_t ring_head;
    uint8_t ring_tail;
    int16_t output_level;
    uint64_t last_flushed_cycle;
    i2s_chan_handle_t i2s_handle;
};

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = { reg, value };

    return i2c_master_write_to_device(ES8311_I2C_PORT,
                                      ES8311_I2C_ADDR,
                                      payload,
                                      sizeof(payload),
                                      pdMS_TO_TICKS(20));
}

static esp_err_t es8311_init(void)
{
    esp_err_t err;

    /* The keyboard driver already initializes I2C_NUM_0 on the ADV.
       Reuse the existing bus — do not call i2c_driver_install again
       (ESP-IDF v5.4 returns ESP_FAIL for duplicate installs). */

    /* Probe the codec — if the first write NACKs, the ES8311 is not
       present or not responding.  Bail out to avoid disrupting the
       shared I2C bus (TCA8418 keyboard controller). */
    err = es8311_write_reg(ES8311_REG_RESET, 0x80U);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 not responding at 0x%02X: %s",
                 ES8311_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    /* Clock manager: derive MCLK from BCLK (no MCLK pin on ADV),
       enable MCLK/BCLK/DAC clocks, disable ADC clocks. */
    err = es8311_write_reg(ES8311_REG_CLK_MANAGER1, 0xB5U);
    if (err != ESP_OK) {
        return err;
    }

    /* PLL: pre_div=1, pre_multi=8x (multiply BCLK by 8). */
    err = es8311_write_reg(ES8311_REG_CLK_MANAGER2, 0x18U);
    if (err != ESP_OK) {
        return err;
    }

    /* Power up analog circuitry. */
    err = es8311_write_reg(ES8311_REG_SYSTEM, 0x01U);
    if (err != ESP_OK) {
        return err;
    }

    /* Power up DAC. */
    err = es8311_write_reg(ES8311_REG_SYSTEM2, 0x00U);
    if (err != ESP_OK) {
        return err;
    }

    /* Enable headphone driver output. */
    err = es8311_write_reg(ES8311_REG_SYSTEM3, 0x10U);
    if (err != ESP_OK) {
        return err;
    }

    /* DAC volume: 0 dB. */
    err = es8311_write_reg(ES8311_REG_DAC1, 0xBFU);
    if (err != ESP_OK) {
        return err;
    }

    /* Bypass DAC equalizer. */
    err = es8311_write_reg(ES8311_REG_DAC2, 0x08U);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}
#endif /* CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV */

cardputer_audio_t *cardputer_audio_init(void)
{
    cardputer_audio_t *audio = calloc(1U, sizeof(*audio));

    if (audio == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio struct");
        return NULL;
    }

    audio->output_level = AUDIO_LEVEL_HIGH;

    /* Configure I2S standard mode: 16-bit mono. */
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
#else
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
#endif
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &audio->i2s_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        free(audio);
        return NULL;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_M5APPLE2_AUDIO_SAMPLE_RATE),
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
#else
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
#endif
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)AUDIO_I2S_BCK_GPIO,
            .ws = (gpio_num_t)AUDIO_I2S_WS_GPIO,
            .dout = (gpio_num_t)AUDIO_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(audio->i2s_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(audio->i2s_handle);
        free(audio);
        return NULL;
    }

    err = i2s_channel_enable(audio->i2s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(audio->i2s_handle);
        free(audio);
        return NULL;
    }

    /* ES8311 codec init must happen AFTER I2S is enabled so
       the PLL can lock onto the running BCLK signal. */
#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
    err = es8311_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed: %s (trying without codec)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ES8311 codec initialized");
    }
#endif

    ESP_LOGI(TAG, "Audio output ready: %d Hz, buffer %d samples",
             CONFIG_M5APPLE2_AUDIO_SAMPLE_RATE,
             CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES);
    return audio;
}

void cardputer_audio_toggle(cardputer_audio_t *audio, uint64_t cpu_cycle)
{
    if (audio == NULL) {
        return;
    }

    uint8_t next_head = (uint8_t)((audio->ring_head + 1U) % AUDIO_TOGGLE_RING_SIZE);

    if (next_head == audio->ring_tail) {
        /* Ring full: drop oldest entry. */
        audio->ring_tail = (uint8_t)((audio->ring_tail + 1U) % AUDIO_TOGGLE_RING_SIZE);
    }

    audio->toggle_ring[audio->ring_head] = cpu_cycle;
    audio->ring_head = next_head;
}

void cardputer_audio_flush(cardputer_audio_t *audio, uint64_t cpu_cycle, uint32_t cpu_hz)
{
    if (audio == NULL || cpu_hz == 0U) {
        return;
    }

    const uint64_t start_cycle = audio->last_flushed_cycle;
    if (cpu_cycle <= start_cycle) {
        return;
    }

    const uint64_t total_cycles = cpu_cycle - start_cycle;
    const uint32_t cycles_per_sample = cpu_hz / CONFIG_M5APPLE2_AUDIO_SAMPLE_RATE;

    if (cycles_per_sample == 0U) {
        audio->last_flushed_cycle = cpu_cycle;
        return;
    }

    uint32_t num_samples = (uint32_t)(total_cycles / cycles_per_sample);
    if (num_samples == 0U) {
        return;
    }
    if (num_samples > CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES) {
        num_samples = CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES;
    }

#if CONFIG_M5APPLE2_CARDPUTER_VARIANT_ADV
    /* Stereo: duplicate each mono sample into L + R channels. */
    int16_t sample_buf[CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES * 2];

    for (uint32_t i = 0; i < num_samples; ++i) {
        const uint64_t sample_end_cycle = start_cycle + (uint64_t)(i + 1U) * cycles_per_sample;

        /* Consume any toggle events that fall within this sample. */
        while (audio->ring_tail != audio->ring_head) {
            const uint64_t toggle_cycle = audio->toggle_ring[audio->ring_tail];

            if (toggle_cycle > sample_end_cycle) {
                break;
            }

            audio->output_level = (audio->output_level == AUDIO_LEVEL_HIGH)
                                      ? AUDIO_LEVEL_LOW
                                      : AUDIO_LEVEL_HIGH;
            audio->ring_tail = (uint8_t)((audio->ring_tail + 1U) % AUDIO_TOGGLE_RING_SIZE);
        }

        sample_buf[i * 2]     = audio->output_level;
        sample_buf[i * 2 + 1] = audio->output_level;
    }

    size_t bytes_written = 0;

    i2s_channel_write(audio->i2s_handle,
                      sample_buf,
                      num_samples * 2U * sizeof(int16_t),
                      &bytes_written,
                      0);
#else
    int16_t sample_buf[CONFIG_M5APPLE2_AUDIO_BUFFER_SAMPLES];

    for (uint32_t i = 0; i < num_samples; ++i) {
        const uint64_t sample_end_cycle = start_cycle + (uint64_t)(i + 1U) * cycles_per_sample;

        /* Consume any toggle events that fall within this sample. */
        while (audio->ring_tail != audio->ring_head) {
            const uint64_t toggle_cycle = audio->toggle_ring[audio->ring_tail];

            if (toggle_cycle > sample_end_cycle) {
                break;
            }

            audio->output_level = (audio->output_level == AUDIO_LEVEL_HIGH)
                                      ? AUDIO_LEVEL_LOW
                                      : AUDIO_LEVEL_HIGH;
            audio->ring_tail = (uint8_t)((audio->ring_tail + 1U) % AUDIO_TOGGLE_RING_SIZE);
        }

        sample_buf[i] = audio->output_level;
    }

    size_t bytes_written = 0;

    i2s_channel_write(audio->i2s_handle,
                      sample_buf,
                      num_samples * sizeof(int16_t),
                      &bytes_written,
                      0);
#endif

    audio->last_flushed_cycle = start_cycle + (uint64_t)num_samples * cycles_per_sample;
}

void cardputer_audio_deinit(cardputer_audio_t *audio)
{
    if (audio == NULL) {
        return;
    }

    i2s_channel_disable(audio->i2s_handle);
    i2s_del_channel(audio->i2s_handle);
    free(audio);
}

#else /* !ESP_PLATFORM */

cardputer_audio_t *cardputer_audio_init(void)
{
    return NULL;
}

void cardputer_audio_toggle(cardputer_audio_t *audio, uint64_t cpu_cycle)
{
    (void)audio;
    (void)cpu_cycle;
}

void cardputer_audio_flush(cardputer_audio_t *audio, uint64_t cpu_cycle, uint32_t cpu_hz)
{
    (void)audio;
    (void)cpu_cycle;
    (void)cpu_hz;
}

void cardputer_audio_deinit(cardputer_audio_t *audio)
{
    (void)audio;
}

#endif /* ESP_PLATFORM */
