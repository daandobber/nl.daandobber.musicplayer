#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_ANALYSIS_BANDS 12
#define AUDIO_ANALYSIS_WAVEFORM 256

typedef struct {
    float    rms;
    float    peak;
    float    bass;
    float    mid;
    float    treble;
    float    bands[AUDIO_ANALYSIS_BANDS];
    int16_t  waveform[AUDIO_ANALYSIS_WAVEFORM];
    float    beat_strength;
    uint32_t beat_counter;
} audio_analysis_snapshot_t;

esp_err_t audio_analysis_init(void);
void      audio_analysis_feed_s16(const int16_t *pcm, size_t frames, uint8_t channels, uint32_t sample_rate);
void      audio_analysis_get(audio_analysis_snapshot_t *out);
void      audio_analysis_reset(void);
