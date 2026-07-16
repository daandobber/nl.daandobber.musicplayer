#include "app_settings.h"

#include <string.h>
#include "nvs.h"

#define SETTINGS_MAGIC 0x4d503231u

static const app_settings_t defaults = {
    .magic = SETTINGS_MAGIC,
    .auto_effect_mode = AUTO_EFFECT_TIME,
    .shuffle_effects = 0,
    .brightness = 80,
    .dim_brightness = 20,
    .visual_intensity = 1,
    .auto_seconds = 45,
    .auto_beats = 64,
    .dim_timeout_seconds = 120,
    .fixed_effect = 122,
    .palette_mode = 1,
    .palette_index = 0,
    .palette_speed = 2,
    .lastfm_enabled = 0,
};

void app_settings_load(app_settings_t *settings) {
    *settings = defaults;
    nvs_handle_t handle;
    if (nvs_open("musicplayer", NVS_READONLY, &handle) != ESP_OK) return;
    app_settings_t stored = defaults;
    size_t size = sizeof(stored);
    if (nvs_get_blob(handle, "settings", &stored, &size) == ESP_OK && size >= 16 &&
        size <= sizeof(stored) && stored.magic == SETTINGS_MAGIC) {
        *settings = stored;
    }
    nvs_close(handle);
}

void app_settings_save(const app_settings_t *settings) {
    nvs_handle_t handle;
    if (nvs_open("musicplayer", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, "settings", settings, sizeof(*settings)) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}
