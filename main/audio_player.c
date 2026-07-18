#include "audio_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "audio_analysis.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jellyfin_client.h"
#include "wifi_setup.h"

#define INPUT_CHUNK_SIZE 4096
#define INITIAL_PCM_SIZE 8192
#define I2S_WRITE_SAMPLES 1024
#define FLAC_STREAMINFO_SIZE 34
#define FLAC_PREFIX_SIZE (4 + 4 + FLAC_STREAMINFO_SIZE)
#define FLAC_MAX_METADATA_BLOCKS 128

static const char *TAG = "player";

typedef enum {
    PLAYER_COMMAND_PLAY,
    PLAYER_COMMAND_TOGGLE_PAUSE,
    PLAYER_COMMAND_STOP,
} player_command_type_t;

typedef struct {
    player_command_type_t type;
    char                  path[AUDIO_PLAYER_PATH_MAX];
} player_command_t;

typedef enum {
    CONTROL_CONTINUE,
    CONTROL_STOP,
    CONTROL_REPLACE,
} control_result_t;

static QueueHandle_t     command_queue;
static QueueHandle_t     event_queue;
static SemaphoreHandle_t snapshot_mutex;
static i2s_chan_handle_t i2s_tx;
static uint32_t          i2s_sample_rate = 44100;
static audio_player_snapshot_t player_snapshot = {.volume = 55};

static void snapshot_set_state(audio_player_state_t state) {
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    player_snapshot.state = state;
    xSemaphoreGive(snapshot_mutex);
}

static void snapshot_start(const char *path) {
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    player_snapshot.state = AUDIO_PLAYER_PLAYING;
    player_snapshot.sample_rate = 0;
    player_snapshot.channels = 0;
    player_snapshot.bits_per_sample = 0;
    player_snapshot.elapsed_seconds = 0;
    player_snapshot.error[0] = '\0';
    strlcpy(player_snapshot.path, path, sizeof(player_snapshot.path));
    xSemaphoreGive(snapshot_mutex);
}

static void snapshot_set_format(const esp_audio_simple_dec_info_t *info) {
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    player_snapshot.sample_rate = info->sample_rate;
    player_snapshot.channels = info->channel;
    player_snapshot.bits_per_sample = info->bits_per_sample;
    xSemaphoreGive(snapshot_mutex);
}

static void snapshot_set_elapsed(uint64_t frames, uint32_t sample_rate) {
    if (sample_rate == 0) return;
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    player_snapshot.elapsed_seconds = (uint32_t)(frames / sample_rate);
    xSemaphoreGive(snapshot_mutex);
}

static void send_event(audio_player_event_type_t type, const char *message) {
    audio_player_event_t event = {.type = type};
    if (message) strlcpy(event.message, message, sizeof(event.message));
    xQueueSend(event_queue, &event, 0);
}

static void set_error(const char *message) {
    ESP_LOGE(TAG, "%s", message);
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    player_snapshot.state = AUDIO_PLAYER_ERROR;
    strlcpy(player_snapshot.error, message, sizeof(player_snapshot.error));
    xSemaphoreGive(snapshot_mutex);
    send_event(AUDIO_PLAYER_EVENT_ERROR, message);
}

static esp_audio_simple_dec_type_t decoder_type_for_path(const char *path) {
    if (jellyfin_client_is_path(path)) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    const char *extension = strrchr(path, '.');
    if (extension && strcasecmp(extension, ".mp3") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (extension && strcasecmp(extension, ".wav") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (extension && strcasecmp(extension, ".flac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (extension && strcasecmp(extension, ".ogg") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

typedef struct {
    FILE *file;
    esp_http_client_handle_t http;
    bool is_http;
    long file_size;
    uint8_t flac_prefix[FLAC_PREFIX_SIZE];
    size_t flac_prefix_length;
    size_t flac_prefix_offset;
} player_input_t;

static bool prepare_flac_input(player_input_t *input) {
    uint8_t marker[4];
    uint8_t block_header[4];
    rewind(input->file);
    if (fread(marker, 1, sizeof(marker), input->file) != sizeof(marker) || memcmp(marker, "fLaC", 4) != 0 ||
        fread(block_header, 1, sizeof(block_header), input->file) != sizeof(block_header)) {
        rewind(input->file);
        return false;
    }

    uint8_t block_type = block_header[0] & 0x7f;
    uint32_t block_size = ((uint32_t)block_header[1] << 16) | ((uint32_t)block_header[2] << 8) | block_header[3];
    if (block_type != 0 || block_size != FLAC_STREAMINFO_SIZE) {
        rewind(input->file);
        return false;
    }

    memcpy(input->flac_prefix, marker, sizeof(marker));
    memcpy(input->flac_prefix + sizeof(marker), block_header, sizeof(block_header));
    if (fread(input->flac_prefix + 8, 1, FLAC_STREAMINFO_SIZE, input->file) != FLAC_STREAMINFO_SIZE) {
        rewind(input->file);
        return false;
    }

    bool last_block = (block_header[0] & 0x80) != 0;
    for (size_t blocks = 1; !last_block && blocks < FLAC_MAX_METADATA_BLOCKS; blocks++) {
        if (fread(block_header, 1, sizeof(block_header), input->file) != sizeof(block_header)) {
            rewind(input->file);
            return false;
        }
        last_block = (block_header[0] & 0x80) != 0;
        block_size = ((uint32_t)block_header[1] << 16) | ((uint32_t)block_header[2] << 8) | block_header[3];
        long block_start = ftell(input->file);
        if (block_start < 0 ||
            (input->file_size >= 0 && (block_start > input->file_size ||
                                      (long)block_size > input->file_size - block_start)) ||
            fseek(input->file, (long)block_size, SEEK_CUR) != 0) {
            rewind(input->file);
            return false;
        }
    }
    if (!last_block) {
        rewind(input->file);
        return false;
    }

    long audio_offset = ftell(input->file);
    if (audio_offset < 0 || (input->file_size >= 0 && audio_offset >= input->file_size)) {
        rewind(input->file);
        return false;
    }

    // The Espressif FLAC elementary-stream parser searches at most 512 kB
    // after STREAMINFO for its first audio frame. Present a valid minimal
    // FLAC header followed directly by audio frames, omitting comments,
    // pictures, padding and seek tables that the decoder does not need.
    input->flac_prefix[4] = 0x80;  // STREAMINFO is now the last metadata block.
    input->flac_prefix_length = FLAC_PREFIX_SIZE;
    input->flac_prefix_offset = 0;
    if (audio_offset > FLAC_PREFIX_SIZE) {
        ESP_LOGI(TAG, "FLAC: skipped %ld bytes of non-audio metadata", audio_offset - FLAC_PREFIX_SIZE);
    }
    return true;
}

static esp_err_t input_open(const char *path, player_input_t *input) {
    memset(input, 0, sizeof(*input));
    input->file_size = -1;
    if (!jellyfin_client_is_path(path)) {
        input->file = fopen(path, "rb");
        if (input->file == NULL) return ESP_FAIL;
        if (fseek(input->file, 0, SEEK_END) == 0) {
            input->file_size = ftell(input->file);
            rewind(input->file);
        }
        if (decoder_type_for_path(path) == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC) prepare_flac_input(input);
        return ESP_OK;
    }

    if (!wifi_setup_connect_blocking(20000)) return ESP_ERR_TIMEOUT;
    char url[512];
    esp_err_t err = jellyfin_client_stream_url(path, url, sizeof(url));
    if (err != ESP_OK) {
        wifi_setup_disconnect();
        return err;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = INPUT_CHUNK_SIZE,
    };
    input->http = esp_http_client_init(&config);
    if (input->http == NULL) {
        wifi_setup_disconnect();
        return ESP_FAIL;
    }
    esp_http_client_set_header(input->http, "Accept", "audio/mpeg,audio/*");
    esp_http_client_set_header(input->http, "X-Emby-Token", jellyfin_client_token());
    err = esp_http_client_open(input->http, 0);
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(input->http);
        int status = esp_http_client_get_status_code(input->http);
        if (status < 200 || status >= 300) err = ESP_FAIL;
        ESP_LOGI(TAG, "Jellyfin stream HTTP %d, %d bytes", status, content_length);
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(input->http);
        input->http = NULL;
        wifi_setup_disconnect();
        return err;
    }
    input->is_http = true;
    return ESP_OK;
}

static size_t input_read(player_input_t *input, uint8_t *buffer, size_t size, bool *failed) {
    *failed = false;
    if (!input->is_http) {
        size_t total = 0;
        if (input->flac_prefix_offset < input->flac_prefix_length) {
            size_t remaining = input->flac_prefix_length - input->flac_prefix_offset;
            size_t take = remaining < size ? remaining : size;
            memcpy(buffer, input->flac_prefix + input->flac_prefix_offset, take);
            input->flac_prefix_offset += take;
            total += take;
        }
        if (total < size) total += fread(buffer + total, 1, size - total, input->file);
        if (ferror(input->file)) *failed = true;
        return total;
    }
    int read = esp_http_client_read(input->http, (char *)buffer, (int)size);
    if (read < 0) {
        *failed = true;
        return 0;
    }
    return (size_t)read;
}

static bool input_eof(player_input_t *input) {
    if (input->is_http) return esp_http_client_is_complete_data_received(input->http);
    if (input->flac_prefix_offset < input->flac_prefix_length) return false;
    if (input->file_size >= 0) {
        long position = ftell(input->file);
        if (position >= 0) return position >= input->file_size;
    }
    return feof(input->file);
}

static void input_close(player_input_t *input) {
    if (input->file) fclose(input->file);
    if (input->http) {
        esp_http_client_close(input->http);
        esp_http_client_cleanup(input->http);
        wifi_setup_disconnect();
    }
}

static control_result_t handle_commands(bool wait, bool *paused, char *replacement) {
    player_command_t command;
    TickType_t timeout = wait ? portMAX_DELAY : 0;
    if (xQueueReceive(command_queue, &command, timeout) != pdTRUE) return CONTROL_CONTINUE;

    switch (command.type) {
        case PLAYER_COMMAND_PLAY:
            strlcpy(replacement, command.path, AUDIO_PLAYER_PATH_MAX);
            return CONTROL_REPLACE;
        case PLAYER_COMMAND_STOP:
            *paused = false;
            snapshot_set_state(AUDIO_PLAYER_STOPPED);
            return CONTROL_STOP;
        case PLAYER_COMMAND_TOGGLE_PAUSE:
            *paused = !*paused;
            snapshot_set_state(*paused ? AUDIO_PLAYER_PAUSED : AUDIO_PLAYER_PLAYING);
            return CONTROL_CONTINUE;
    }
    return CONTROL_CONTINUE;
}

static control_result_t wait_while_paused(bool *paused, char *replacement) {
    while (*paused) {
        control_result_t control = handle_commands(true, paused, replacement);
        if (control != CONTROL_CONTINUE) return control;
    }
    return CONTROL_CONTINUE;
}

static esp_err_t write_i2s(const void *data, size_t bytes) {
    // The ES8156 + onboard amplifier clips on modern 0 dBFS masters even when
    // the codec volume is turned down. Keep 6 dB of digital headroom, matching
    // the proven audio path used by the other Tanmatsu media applications.
    static int16_t scratch[I2S_WRITE_SAMPLES];
    const int16_t *cursor = data;
    size_t samples = bytes / sizeof(int16_t);
    while (samples > 0) {
        size_t batch = samples > I2S_WRITE_SAMPLES ? I2S_WRITE_SAMPLES : samples;
        for (size_t i = 0; i < batch; i++) scratch[i] = (int16_t)((int32_t)cursor[i] / 2);

        const uint8_t *output = (const uint8_t *)scratch;
        size_t remaining = batch * sizeof(int16_t);
        while (remaining > 0) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(i2s_tx, output, remaining, &written, 1000);
            if (err != ESP_OK) return err;
            if (written == 0) return ESP_ERR_TIMEOUT;
            output += written;
            remaining -= written;
        }
        cursor += batch;
        samples -= batch;
    }
    return ESP_OK;
}

static esp_err_t configure_i2s_rate(uint32_t sample_rate) {
    if (sample_rate == i2s_sample_rate) return ESP_OK;
    esp_err_t err = i2s_channel_disable(i2s_tx);
    if (err != ESP_OK) return err;
    err = bsp_audio_set_rate(sample_rate);
    esp_err_t enable_err = i2s_channel_enable(i2s_tx);
    if (err == ESP_OK) err = enable_err;
    if (err == ESP_OK) i2s_sample_rate = sample_rate;
    return err;
}

static int16_t pcm_sample_to_s16(const uint8_t *sample, uint8_t bits) {
    switch (bits) {
        case 8:
            return ((int16_t)sample[0] - 128) << 8;
        case 16:
            return (int16_t)((uint16_t)sample[0] | ((uint16_t)sample[1] << 8));
        case 24: {
            int32_t value = (int32_t)((uint32_t)sample[0] | ((uint32_t)sample[1] << 8) |
                                      ((uint32_t)sample[2] << 16));
            if (value & 0x00800000) value |= (int32_t)0xff000000;
            return (int16_t)(value >> 8);
        }
        case 32: {
            int32_t value = (int32_t)((uint32_t)sample[0] | ((uint32_t)sample[1] << 8) |
                                      ((uint32_t)sample[2] << 16) | ((uint32_t)sample[3] << 24));
            return (int16_t)(value >> 16);
        }
        default:
            return 0;
    }
}

static esp_err_t output_pcm(const uint8_t *data, size_t bytes, const esp_audio_simple_dec_info_t *info,
                            int16_t **stereo_buffer, size_t *stereo_capacity, uint64_t *total_frames) {
    if ((info->bits_per_sample != 8 && info->bits_per_sample != 16 && info->bits_per_sample != 24 &&
         info->bits_per_sample != 32) ||
        (info->channel != 1 && info->channel != 2)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t bytes_per_sample = info->bits_per_sample / 8;
    size_t frames = bytes / (bytes_per_sample * info->channel);

    esp_err_t err;
    if (info->channel == 2 && info->bits_per_sample == 16) {
        const int16_t *samples = (const int16_t *)data;
        audio_analysis_feed_s16(samples, frames, 2, info->sample_rate);
        err = write_i2s(data, frames * 2 * sizeof(int16_t));
    } else {
        size_t needed = frames * 2 * sizeof(int16_t);
        if (*stereo_capacity < needed) {
            int16_t *larger = realloc(*stereo_buffer, needed);
            if (larger == NULL) return ESP_ERR_NO_MEM;
            *stereo_buffer = larger;
            *stereo_capacity = needed;
        }
        for (size_t frame = 0; frame < frames; frame++) {
            const uint8_t *source = data + frame * info->channel * bytes_per_sample;
            int16_t left = pcm_sample_to_s16(source, info->bits_per_sample);
            int16_t right = info->channel == 2 ? pcm_sample_to_s16(source + bytes_per_sample, info->bits_per_sample)
                                               : left;
            (*stereo_buffer)[frame * 2] = left;
            (*stereo_buffer)[frame * 2 + 1] = right;
        }
        audio_analysis_feed_s16(*stereo_buffer, frames, 2, info->sample_rate);
        err = write_i2s(*stereo_buffer, needed);
    }
    *total_frames += frames;
    snapshot_set_elapsed(*total_frames, info->sample_rate);
    return err;
}

static control_result_t decode_file(const char *path, char *replacement) {
    player_input_t source;
    if (input_open(path, &source) != ESP_OK) {
        set_error("Unable to open the audio file");
        return CONTROL_STOP;
    }

    esp_audio_simple_dec_type_t type = decoder_type_for_path(path);
    uint64_t decoder_config[8] = {0};
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = type,
        .dec_cfg = decoder_config,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_err_t audio_err = esp_audio_simple_dec_open(&config, &decoder);
    if (audio_err != ESP_AUDIO_ERR_OK) {
        input_close(&source);
        set_error("The decoder cannot open this file");
        return CONTROL_STOP;
    }

    uint8_t *input = malloc(INPUT_CHUNK_SIZE);
    size_t pcm_capacity = INITIAL_PCM_SIZE;
    uint8_t *pcm = malloc(pcm_capacity);
    int16_t *stereo = NULL;
    size_t stereo_capacity = 0;
    if (input == NULL || pcm == NULL) {
        free(input);
        free(pcm);
        esp_audio_simple_dec_close(decoder);
        input_close(&source);
        set_error("Onvoldoende geheugen voor decoder");
        return CONTROL_STOP;
    }

    snapshot_start(path);
    audio_analysis_reset();
    bool paused = false;
    bool format_ready = false;
    esp_audio_simple_dec_info_t info = {0};
    uint64_t total_frames = 0;
    control_result_t result = CONTROL_CONTINUE;
    bool failed = false;

    while (!failed) {
        result = handle_commands(false, &paused, replacement);
        if (result != CONTROL_CONTINUE) break;
        result = wait_while_paused(&paused, replacement);
        if (result != CONTROL_CONTINUE) break;

        bool read_failed = false;
        size_t read = input_read(&source, input, INPUT_CHUNK_SIZE, &read_failed);
        if (read == 0) {
            if (read_failed) {
                set_error(source.is_http ? "Network stream read error" : "SD card read error");
                failed = true;
            }
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input,
            .len = read,
            .eos = input_eof(&source),
        };

        while (raw.len > 0) {
            esp_audio_simple_dec_out_t output = {
                .buffer = pcm,
                .len = pcm_capacity,
            };
            raw.consumed = 0;
            audio_err = esp_audio_simple_dec_process(decoder, &raw, &output);
            if (audio_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *larger = realloc(pcm, output.needed_size);
                if (larger == NULL) {
                    set_error("Unable to grow the PCM buffer");
                    failed = true;
                    break;
                }
                pcm = larger;
                pcm_capacity = output.needed_size;
                continue;
            }
            if (audio_err != ESP_AUDIO_ERR_OK) {
                set_error("Ongeldige of beschadigde audiogegevens");
                failed = true;
                break;
            }

            if (output.decoded_size > 0) {
                esp_audio_simple_dec_info_t latest = {0};
                if (esp_audio_simple_dec_get_info(decoder, &latest) != ESP_AUDIO_ERR_OK) {
                    set_error("Audioformaat ontbreekt");
                    failed = true;
                    break;
                }
                if (!format_ready || latest.sample_rate != info.sample_rate || latest.channel != info.channel ||
                    latest.bits_per_sample != info.bits_per_sample) {
                    info = latest;
                    if ((info.bits_per_sample != 8 && info.bits_per_sample != 16 && info.bits_per_sample != 24 &&
                         info.bits_per_sample != 32) ||
                        (info.channel != 1 && info.channel != 2)) {
                        set_error("WAV must be 8/16/24/32-bit mono or stereo");
                        failed = true;
                        break;
                    }
                    if (configure_i2s_rate(info.sample_rate) != ESP_OK) {
                        set_error("Unable to configure the I2S sample rate");
                        failed = true;
                        break;
                    }
                    snapshot_set_format(&info);
                    format_ready = true;
                    ESP_LOGI(TAG, "%s: %lu Hz, %u channels", path, (unsigned long)info.sample_rate, info.channel);
                }
                esp_err_t err = output_pcm(output.buffer, output.decoded_size, &info, &stereo, &stereo_capacity,
                                           &total_frames);
                if (err != ESP_OK) {
                    set_error(err == ESP_ERR_NOT_SUPPORTED ? "Unsupported PCM format" : "I2S output failed");
                    failed = true;
                    break;
                }
            }

            // A simple decoder may first return a frame that was assembled
            // from its internal cache without consuming the newly supplied
            // input. Keep offering that same input until it is consumed;
            // overwriting it with the next SD-card chunk corrupts frame sync,
            // especially for FLAC frames larger than INPUT_CHUNK_SIZE.
            if (raw.consumed == 0 && output.decoded_size == 0) {
                set_error("Decoder made no progress");
                failed = true;
                break;
            }
            if (raw.consumed > 0) {
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }

            result = handle_commands(false, &paused, replacement);
            if (result != CONTROL_CONTINUE) break;
        }
        if (result != CONTROL_CONTINUE) break;
    }

    free(stereo);
    free(pcm);
    free(input);
    esp_audio_simple_dec_close(decoder);
    input_close(&source);

    if (result == CONTROL_REPLACE) return result;
    if (!failed && result != CONTROL_STOP) {
        snapshot_set_state(AUDIO_PLAYER_STOPPED);
        send_event(AUDIO_PLAYER_EVENT_FINISHED, NULL);
    }
    return CONTROL_STOP;
}

static void player_task(void *unused) {
    (void)unused;
    char current[AUDIO_PLAYER_PATH_MAX] = {0};
    while (true) {
        if (current[0] == '\0') {
            player_command_t command;
            if (xQueueReceive(command_queue, &command, portMAX_DELAY) != pdTRUE) continue;
            if (command.type != PLAYER_COMMAND_PLAY) continue;
            strlcpy(current, command.path, sizeof(current));
        }

        char replacement[AUDIO_PLAYER_PATH_MAX] = {0};
        control_result_t result = decode_file(current, replacement);
        if (result == CONTROL_REPLACE) {
            strlcpy(current, replacement, sizeof(current));
        } else {
            current[0] = '\0';
        }
    }
}

esp_err_t audio_player_init(void) {
    command_queue = xQueueCreate(6, sizeof(player_command_t));
    event_queue = xQueueCreate(4, sizeof(audio_player_event_t));
    snapshot_mutex = xSemaphoreCreateMutex();
    if (command_queue == NULL || event_queue == NULL || snapshot_mutex == NULL) return ESP_ERR_NO_MEM;

    esp_audio_err_t audio_err = esp_audio_dec_register_default();
    if (audio_err != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    audio_err = esp_audio_simple_dec_register_default();
    if (audio_err != ESP_AUDIO_ERR_OK) return ESP_FAIL;

    esp_err_t err = bsp_audio_get_i2s_handle(&i2s_tx);
    if (err != ESP_OK || i2s_tx == NULL) return err == ESP_OK ? ESP_FAIL : err;
    ESP_RETURN_ON_ERROR(audio_analysis_init(), TAG, "Audio analysis init failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(player_snapshot.volume), TAG, "Volume init failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_amplifier(true), TAG, "Amplifier init failed");

    // Keep decoding and FFT work on core 1 so core 0 remains available for the
    // display/UI loop. Audio has priority over the lower-priority analyzer.
    if (xTaskCreatePinnedToCore(player_task, "audio-player", 10240, NULL, 8, NULL, 1) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t audio_player_play(const char *path) {
    if (command_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (path == NULL || strlen(path) >= AUDIO_PLAYER_PATH_MAX) return ESP_ERR_INVALID_ARG;
    player_command_t command = {.type = PLAYER_COMMAND_PLAY};
    strlcpy(command.path, path, sizeof(command.path));
    return xQueueSend(command_queue, &command, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_toggle_pause(void) {
    if (command_queue == NULL) return ESP_ERR_INVALID_STATE;
    player_command_t command = {.type = PLAYER_COMMAND_TOGGLE_PAUSE};
    return xQueueSend(command_queue, &command, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_stop(void) {
    if (command_queue == NULL) return ESP_ERR_INVALID_STATE;
    player_command_t command = {.type = PLAYER_COMMAND_STOP};
    return xQueueSend(command_queue, &command, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_set_volume(uint8_t percentage) {
    if (snapshot_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (percentage > 100) percentage = 100;
    esp_err_t err = bsp_audio_set_volume(percentage);
    if (err == ESP_OK) {
        xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
        player_snapshot.volume = percentage;
        xSemaphoreGive(snapshot_mutex);
    }
    return err;
}

void audio_player_get_snapshot(audio_player_snapshot_t *out) {
    if (out == NULL) return;
    if (snapshot_mutex == NULL) {
        *out = player_snapshot;
        return;
    }
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    *out = player_snapshot;
    xSemaphoreGive(snapshot_mutex);
}

bool audio_player_poll_event(audio_player_event_t *out) {
    if (out == NULL || event_queue == NULL) return false;
    return xQueueReceive(event_queue, out, 0) == pdTRUE;
}
