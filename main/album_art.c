#include "album_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "jpeg_decoder.h"

#define MAX_JPEG_BYTES (8 * 1024 * 1024)
#define MAX_COVER_SIDE 2048

static const char *TAG = "album_art";
static uint16_t *scaled_pixels;
static int scaled_size;
static char cached_path[256];
static uint32_t cached_offset;
static uint32_t cached_length;

void album_art_invalidate(void) {
    cached_path[0] = '\0';
    cached_offset = 0;
    cached_length = 0;
    scaled_size = 0;
}

static bool load_cover(const media_track_t *track, int target_size) {
    if (!track || !track->cover_path || !track->cover_size || track->cover_size > MAX_JPEG_BYTES) return false;
    if (scaled_pixels && scaled_size == target_size && cached_offset == track->cover_offset &&
        cached_length == track->cover_size && strcmp(cached_path, track->cover_path) == 0) return true;

    uint8_t *input = malloc(track->cover_size);
    if (!input) return false;
    FILE *file = fopen(track->cover_path, "rb");
    bool read_ok = file && fseek(file, track->cover_offset, SEEK_SET) == 0 &&
                   fread(input, 1, track->cover_size, file) == track->cover_size;
    if (file) fclose(file);
    if (!read_ok) {
        free(input);
        return false;
    }

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = input,
        .indata_size = track->cover_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&jpeg_cfg, &info) != ESP_OK || !info.width || !info.height ||
        info.width > MAX_COVER_SIDE || info.height > MAX_COVER_SIDE) {
        free(input);
        return false;
    }
    uint32_t shortest = info.width < info.height ? info.width : info.height;
    if (shortest / 8 >= (uint32_t)target_size) jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    else if (shortest / 4 >= (uint32_t)target_size) jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    else if (shortest / 2 >= (uint32_t)target_size) jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    if (esp_jpeg_get_image_info(&jpeg_cfg, &info) != ESP_OK || !info.output_len) {
        free(input);
        return false;
    }
    uint16_t *output = malloc(info.output_len);
    if (!output) {
        free(input);
        return false;
    }
    jpeg_cfg.outbuf = (uint8_t *)output;
    jpeg_cfg.outbuf_size = info.output_len;
    // Complex covers can contain enough Huffman/quantization tables to exceed
    // esp_jpeg's conservative 3.1 kB default pool.
    size_t working_size = 64 * 1024;
    void *working = malloc(working_size);
    if (!working) {
        free(output);
        free(input);
        return false;
    }
    jpeg_cfg.advanced.working_buffer = working;
    jpeg_cfg.advanced.working_buffer_size = working_size;
    esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &info);
    free(working);
    free(input);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        free(output);
        return false;
    }

    uint16_t *new_scaled = realloc(scaled_pixels, (size_t)target_size * target_size * sizeof(uint16_t));
    if (!new_scaled) {
        free(output);
        return false;
    }
    scaled_pixels = new_scaled;
    uint32_t stride = info.width;
    uint32_t crop = info.width < info.height ? info.width : info.height;
    uint32_t crop_x = (info.width - crop) / 2;
    uint32_t crop_y = (info.height - crop) / 2;
    for (int y = 0; y < target_size; y++) {
        uint32_t source_y = crop_y + (uint32_t)y * crop / target_size;
        for (int x = 0; x < target_size; x++) {
            uint32_t source_x = crop_x + (uint32_t)x * crop / target_size;
            scaled_pixels[y * target_size + x] = output[source_y * stride + source_x];
        }
    }
    free(output);
    scaled_size = target_size;
    cached_offset = track->cover_offset;
    cached_length = track->cover_size;
    strlcpy(cached_path, track->cover_path, sizeof(cached_path));
    ESP_LOGI(TAG, "Cover loaded: %s (%lux%lu)", track->cover_path,
             (unsigned long)info.width, (unsigned long)info.height);
    return true;
}

bool album_art_draw(pax_buf_t *buffer, const media_track_t *track, int x, int y, int size) {
    if (!buffer || size <= 0 || !load_cover(track, size) || buffer->orientation != PAX_O_ROT_CW ||
        !buffer->buf_16bpp) return false;
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            uint16_t source = scaled_pixels[py * size + px];
            uint8_t red = ((source >> 11) & 0x1f) * 255 / 31;
            uint8_t green = ((source >> 5) & 0x3f) * 255 / 63;
            uint8_t blue = (source & 0x1f) * 255 / 31;
            uint16_t pixel = (uint16_t)buffer->col2buf(buffer, pax_col_rgb(red, green, blue));
            buffer->buf_16bpp[(x + px) * buffer->width + (buffer->width - 1 - (y + py))] = pixel;
        }
    }
    return true;
}
