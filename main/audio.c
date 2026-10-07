#include "audio.h"
#include <stdio.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "audio";

#define I2S_BCLK_PIN  GPIO_NUM_20
#define I2S_LRCLK_PIN GPIO_NUM_46
#define I2S_DOUT_PIN  GPIO_NUM_19

// SD card shares the display's 3-wire SPI physical lines (SCK/MOSI) —
// same bus, separate CS pin (SD_SPI_CS) selects the card instead of the panel.
#define SD_SPI_SCK  12
#define SD_SPI_MOSI 11
#define SD_SPI_MISO 13
#define SD_SPI_CS   10

#define SAMPLE_RATE        44100
#define MIX_CHUNK_SAMPLES  256
#define TONE_AMPLITUDE     8000   // a full-volume beep (of 32767): loud enough, leaves room to mix

typedef struct {
    volatile bool active;
    float volume;           // 0.0 (silent) to 1.0 (full) for this channel only
    // WAV (file != NULL)
    FILE *file;
    int loops_left;         // -1 = infinite, else counts down; hits 0 = channel frees itself
    uint32_t data_start;    // file offset where PCM samples begin
    uint32_t data_size;     // total PCM bytes for this file
    uint32_t bytes_read;    // bytes consumed so far
    // beep (file == NULL)
    uint32_t tone_period;   // samples per square-wave cycle
    uint32_t tone_pos;      // samples played so far
    uint32_t tone_len;      // total samples to play
} audio_channel_t;

static i2s_chan_handle_t s_tx_chan = NULL;
static audio_channel_t s_channels[AUDIO_MAX_CHANNELS];
static SemaphoreHandle_t s_channels_mutex = NULL;
static float s_master_volume = 1.0f;

typedef struct {
    uint32_t data_offset;
    uint32_t data_size;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
} wav_info_t;

static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Walks a WAV file's RIFF chunks to find "fmt " and "data" — more robust
// than assuming a fixed 44-byte header, since some WAV files have extra
// chunks (metadata, etc.) before the actual audio data.
static bool wav_parse_header(FILE *f, wav_info_t *out)
{
    char riff[4], wave[4];
    uint32_t chunk_size;
    if (fread(riff, 1, 4, f) != 4) return false;
    if (fread(&chunk_size, 4, 1, f) != 1) return false;
    if (fread(wave, 1, 4, f) != 4) return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) return false;

    bool got_fmt = false, got_data = false;
    while (!(got_fmt && got_data)) {
        char id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;

        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t audio_format, num_channels, block_align, bits_per_sample;
            uint32_t sample_rate, byte_rate;
            fread(&audio_format, 2, 1, f);
            fread(&num_channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            fread(&byte_rate, 4, 1, f);
            fread(&block_align, 2, 1, f);
            fread(&bits_per_sample, 2, 1, f);
            long extra = (long)sz - 16;
            if (extra > 0) fseek(f, extra, SEEK_CUR);
            out->num_channels = num_channels;
            out->sample_rate = sample_rate;
            out->bits_per_sample = bits_per_sample;
            got_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            out->data_offset = ftell(f);
            out->data_size = sz;
            got_data = true;
            break;
        } else {
            fseek(f, sz, SEEK_CUR); // skip chunk we don't care about
        }
        if (sz % 2 == 1) fseek(f, 1, SEEK_CUR); // chunks are word-aligned
    }
    return got_fmt && got_data;
}

// Puts *ch on a free channel. Returns its handle, or SOUND_HANDLE_INVALID if all are busy.
static sound_handle_t channel_start(const audio_channel_t *ch)
{
    xSemaphoreTake(s_channels_mutex, portMAX_DELAY);
    int free_ch = -1;
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (!s_channels[i].active) { free_ch = i; break; }
    }
    if (free_ch >= 0) {
        s_channels[free_ch] = *ch;
        s_channels[free_ch].active = true;
    }
    xSemaphoreGive(s_channels_mutex);
    return free_ch;
}

sound_handle_t play_sound(const char *filename, int loops)
{
    if (!s_channels_mutex) return SOUND_HANDLE_INVALID;   // audio_begin() never ran

    char path[160];
    snprintf(path, sizeof(path), SOUND_DIR "/%s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "%s: file not found", path);
        return SOUND_HANDLE_INVALID;
    }

    wav_info_t info;
    if (!wav_parse_header(f, &info)) {
        ESP_LOGE(TAG, "%s: not a valid WAV file", filename);
        fclose(f);
        return SOUND_HANDLE_INVALID;
    }
    if (info.num_channels != 1 || info.bits_per_sample != 16) {
        ESP_LOGE(TAG, "%s: must be 16-bit mono (got %d-bit, %d channels)", filename,
                 info.bits_per_sample, info.num_channels);
        fclose(f);
        return SOUND_HANDLE_INVALID;
    }
    fseek(f, info.data_offset, SEEK_SET);

    audio_channel_t ch = {
        .volume = 1.0f,
        .file = f,
        .loops_left = (loops <= 0) ? -1 : loops,   // 0 or negative = infinite
        .data_start = info.data_offset,
        .data_size = info.data_size,
    };
    sound_handle_t h = channel_start(&ch);
    if (h == SOUND_HANDLE_INVALID) {
        ESP_LOGW(TAG, "%s: no free audio channel, sound dropped", filename);
        fclose(f);
    }
    return h;
}

sound_handle_t play_tone(int freq_hz, int duration_ms, float volume)
{
    if (!s_channels_mutex || freq_hz <= 0 || duration_ms <= 0) return SOUND_HANDLE_INVALID;
    audio_channel_t ch = {
        .volume = clamp01(volume),
        .tone_period = SAMPLE_RATE / freq_hz ? SAMPLE_RATE / freq_hz : 1,
        .tone_len = (uint32_t)((uint64_t)SAMPLE_RATE * duration_ms / 1000),
    };
    return channel_start(&ch);
}

// Stops playback on the given handle and frees its channel immediately —
// use this for anything started with loops=0 (infinite), since those
// never stop on their own.
void stop_sound(sound_handle_t handle)
{
    if (handle < 0 || handle >= AUDIO_MAX_CHANNELS || !s_channels_mutex) return;

    xSemaphoreTake(s_channels_mutex, portMAX_DELAY);
    audio_channel_t *c = &s_channels[handle];
    if (c->file) {
        fclose(c->file);
        c->file = NULL;
    }
    c->active = false;
    xSemaphoreGive(s_channels_mutex);
}

// Note: a handle is reused once its sound ends, so this can be true for a
// newer sound that landed on the same channel.
bool sound_playing(sound_handle_t handle)
{
    if (handle < 0 || handle >= AUDIO_MAX_CHANNELS) return false;
    return s_channels[handle].active;
}

void set_sound_volume(sound_handle_t handle, float volume)
{
    if (handle < 0 || handle >= AUDIO_MAX_CHANNELS || !s_channels_mutex) return;

    xSemaphoreTake(s_channels_mutex, portMAX_DELAY);
    s_channels[handle].volume = clamp01(volume);
    xSemaphoreGive(s_channels_mutex);
}

void set_master_volume(float volume)
{
    s_master_volume = clamp01(volume);   // one float write: the mixer picks it up next chunk
}

// Fills up to MIX_CHUNK_SAMPLES of a WAV channel into chunk_buf. Handles
// looping and frees the channel once its loop count reaches zero.
static size_t mix_read_wav(audio_channel_t *c, int ch, int16_t *chunk_buf)
{
    uint32_t remaining = c->data_size - c->bytes_read;
    uint32_t want_bytes = MIX_CHUNK_SAMPLES * sizeof(int16_t);
    uint32_t to_read = (remaining < want_bytes) ? remaining : want_bytes;
    size_t samples_read = 0;

    if (to_read > 0) {
        samples_read = fread(chunk_buf, sizeof(int16_t), to_read / sizeof(int16_t), c->file);
        c->bytes_read += samples_read * sizeof(int16_t);
    }
    if (c->bytes_read >= c->data_size || samples_read == 0) {
        if (c->loops_left < 0 || --c->loops_left > 0) {
            fseek(c->file, c->data_start, SEEK_SET);   // go round again
            c->bytes_read = 0;
        } else {
            fclose(c->file);
            c->file = NULL;
            c->active = false;   // channel is now free for the next sound
            ESP_LOGD(TAG, "channel %d finished", ch);
        }
    }
    return samples_read;
}

// Fills up to MIX_CHUNK_SAMPLES of a beep into chunk_buf.
static size_t mix_read_tone(audio_channel_t *c, int16_t *chunk_buf)
{
    size_t n = 0;
    while (n < MIX_CHUNK_SAMPLES && c->tone_pos < c->tone_len) {
        bool high = (c->tone_pos % c->tone_period) < c->tone_period / 2;
        chunk_buf[n++] = high ? TONE_AMPLITUDE : -TONE_AMPLITUDE;
        c->tone_pos++;
    }
    if (c->tone_pos >= c->tone_len) c->active = false;
    return n;
}

// Runs continuously on Core 1: takes a chunk from every active channel,
// scales each by its own volume, sums them, scales the sum by master
// volume, clamps to avoid wraparound distortion, and writes it to I2S.
static void audio_mixer_task(void *arg)
{
    int16_t chunk_buf[MIX_CHUNK_SAMPLES];
    int32_t mix_buf[MIX_CHUNK_SAMPLES];
    int16_t out_buf[MIX_CHUNK_SAMPLES];

    while (1) {
        memset(mix_buf, 0, sizeof(mix_buf));
        bool any_active = false;

        xSemaphoreTake(s_channels_mutex, portMAX_DELAY);
        for (int ch = 0; ch < AUDIO_MAX_CHANNELS; ch++) {
            audio_channel_t *c = &s_channels[ch];
            if (!c->active) continue;
            any_active = true;

            float vol = c->volume;
            size_t n = c->file ? mix_read_wav(c, ch, chunk_buf) : mix_read_tone(c, chunk_buf);
            for (size_t i = 0; i < n; i++) {
                mix_buf[i] += (int32_t)((float)chunk_buf[i] * vol);
            }
        }
        xSemaphoreGive(s_channels_mutex);

        if (!any_active) {
            vTaskDelay(pdMS_TO_TICKS(10)); // nothing playing right now — idle
            continue;
        }

        float master = s_master_volume;
        for (int i = 0; i < MIX_CHUNK_SAMPLES; i++) {
            int32_t s = (int32_t)((float)mix_buf[i] * master);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out_buf[i] = (int16_t)s;
        }

        size_t bytes_written;
        i2s_channel_write(s_tx_chan, out_buf, sizeof(out_buf), &bytes_written, portMAX_DELAY);
    }
}

bool sd_begin(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_SPI_MOSI,
        .miso_io_num = SD_SPI_MISO,
        .sclk_io_num = SD_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_SPI_CS;
    slot_cfg.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,   // one per audio channel + a couple spare
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed (%s) — WAV files won't play", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    return true;
}

void audio_begin(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    memset(s_channels, 0, sizeof(s_channels));
    s_channels_mutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(audio_mixer_task, "audio_mixer", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "audio up: I2S %d Hz mono, %d channels", SAMPLE_RATE, AUDIO_MAX_CHANNELS);
}
