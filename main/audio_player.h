#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_PLAYER_PATH_MAX 512

typedef enum {
    AUDIO_PLAYER_STOPPED = 0,
    AUDIO_PLAYER_PLAYING,
    AUDIO_PLAYER_PAUSED,
    AUDIO_PLAYER_ERROR,
} audio_player_state_t;

typedef enum {
    AUDIO_PLAYER_EVENT_NONE = 0,
    AUDIO_PLAYER_EVENT_FINISHED,
    AUDIO_PLAYER_EVENT_ERROR,
} audio_player_event_type_t;

typedef struct {
    audio_player_event_type_t type;
    char                      message[96];
} audio_player_event_t;

typedef struct {
    audio_player_state_t state;
    uint32_t             sample_rate;
    uint8_t              channels;
    uint8_t              bits_per_sample;
    uint32_t             elapsed_seconds;
    uint32_t             duration_seconds;
    uint8_t              volume;
    char                 path[AUDIO_PLAYER_PATH_MAX];
    char                 error[96];
} audio_player_snapshot_t;

esp_err_t audio_player_init(void);
esp_err_t audio_player_play(const char *path);
esp_err_t audio_player_toggle_pause(void);
esp_err_t audio_player_stop(void);
esp_err_t audio_player_set_volume(uint8_t percentage);
void      audio_player_get_snapshot(audio_player_snapshot_t *out);
bool      audio_player_poll_event(audio_player_event_t *out);
