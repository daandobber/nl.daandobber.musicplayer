#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "media_library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char url[160];
    char token[128];
    char user_id[64];
    bool has_token;
} jellyfin_config_t;

esp_err_t jellyfin_client_init(void);
bool jellyfin_client_configured(void);
void jellyfin_client_get_config(jellyfin_config_t *out);
esp_err_t jellyfin_client_set_config(const char *url, const char *token, const char *user_id);
esp_err_t jellyfin_client_append_tracks(media_library_t *library);
bool jellyfin_client_is_path(const char *path);
esp_err_t jellyfin_client_stream_url(const char *path, char *out, size_t out_size);
const char *jellyfin_client_token(void);

#ifdef __cplusplus
}
#endif
