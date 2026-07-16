#pragma once

#include <stdint.h>

typedef enum {
    AUTO_EFFECT_OFF = 0,
    AUTO_EFFECT_TIME,
    AUTO_EFFECT_BEATS,
    AUTO_EFFECT_TRACK,
} auto_effect_mode_t;

typedef struct {
    uint32_t magic;
    uint8_t auto_effect_mode;
    uint8_t shuffle_effects;
    uint8_t brightness;
    uint8_t dim_brightness;
    uint8_t visual_intensity;
    uint16_t auto_seconds;
    uint16_t auto_beats;
    uint16_t dim_timeout_seconds;
    uint8_t fixed_effect;
    uint8_t palette_mode;
    uint8_t palette_index;
    uint8_t palette_speed;
    uint8_t lastfm_enabled;
} app_settings_t;

void app_settings_load(app_settings_t *settings);
void app_settings_save(const app_settings_t *settings);
