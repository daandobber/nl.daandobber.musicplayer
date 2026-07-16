#include "jellyfin_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_setup.h"

#define JELLYFIN_PATH_PREFIX "jellyfin://"
#define JELLYFIN_HTTP_CAP (192 * 1024)
#define JELLYFIN_NVS_NAMESPACE "musicplayer_jf"

#ifndef CONFIG_MUSICPLAYER_JELLYFIN_URL
#define CONFIG_MUSICPLAYER_JELLYFIN_URL ""
#endif
#ifndef CONFIG_MUSICPLAYER_JELLYFIN_TOKEN
#define CONFIG_MUSICPLAYER_JELLYFIN_TOKEN ""
#endif
#ifndef CONFIG_MUSICPLAYER_JELLYFIN_USER_ID
#define CONFIG_MUSICPLAYER_JELLYFIN_USER_ID ""
#endif

static const char *TAG = "jellyfin";
static jellyfin_config_t s_config;
static bool s_config_loaded;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} jellyfin_recv_ctx_t;

static void ensure_config_loaded(void) {
    if (s_config_loaded) return;
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.url, CONFIG_MUSICPLAYER_JELLYFIN_URL, sizeof(s_config.url));
    strlcpy(s_config.token, CONFIG_MUSICPLAYER_JELLYFIN_TOKEN, sizeof(s_config.token));
    strlcpy(s_config.user_id, CONFIG_MUSICPLAYER_JELLYFIN_USER_ID, sizeof(s_config.user_id));
    s_config.has_token = s_config.token[0] != '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(JELLYFIN_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len = sizeof(s_config.url);
        if (nvs_get_str(nvs, "url", s_config.url, &len) != ESP_OK) s_config.url[0] = '\0';
        len = sizeof(s_config.token);
        if (nvs_get_str(nvs, "token", s_config.token, &len) != ESP_OK) s_config.token[0] = '\0';
        len = sizeof(s_config.user_id);
        if (nvs_get_str(nvs, "user", s_config.user_id, &len) != ESP_OK) s_config.user_id[0] = '\0';
        nvs_close(nvs);
        s_config.has_token = s_config.token[0] != '\0';
    }
    s_config_loaded = true;
}

esp_err_t jellyfin_client_init(void) {
    ensure_config_loaded();
    return ESP_OK;
}

bool jellyfin_client_configured(void) {
    ensure_config_loaded();
    return s_config.url[0] != '\0' && s_config.token[0] != '\0';
}

bool jellyfin_client_is_path(const char *path) {
    return path != NULL && strncmp(path, JELLYFIN_PATH_PREFIX, strlen(JELLYFIN_PATH_PREFIX)) == 0;
}

const char *jellyfin_client_token(void) {
    ensure_config_loaded();
    return s_config.token;
}

void jellyfin_client_get_config(jellyfin_config_t *out) {
    if (out == NULL) return;
    ensure_config_loaded();
    memset(out, 0, sizeof(*out));
    strlcpy(out->url, s_config.url, sizeof(out->url));
    strlcpy(out->user_id, s_config.user_id, sizeof(out->user_id));
    out->has_token = s_config.has_token;
}

esp_err_t jellyfin_client_set_config(const char *url, const char *token, const char *user_id) {
    ensure_config_loaded();
    jellyfin_config_t next = s_config;
    strlcpy(next.url, url != NULL ? url : "", sizeof(next.url));
    strlcpy(next.user_id, user_id != NULL ? user_id : "", sizeof(next.user_id));
    if (token != NULL && token[0] != '\0') strlcpy(next.token, token, sizeof(next.token));
    if (next.url[0] == '\0') next.token[0] = '\0';
    next.has_token = next.token[0] != '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(JELLYFIN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (next.url[0] != '\0') err = nvs_set_str(nvs, "url", next.url);
    else err = nvs_erase_key(nvs, "url");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        if (next.token[0] != '\0') err = nvs_set_str(nvs, "token", next.token);
        else err = nvs_erase_key(nvs, "token");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        if (next.user_id[0] != '\0') err = nvs_set_str(nvs, "user", next.user_id);
        else err = nvs_erase_key(nvs, "user");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) s_config = next;
    return err;
}

static void base_url(char *out, size_t out_size) {
    ensure_config_loaded();
    strlcpy(out, s_config.url, out_size);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        jellyfin_recv_ctx_t *ctx = (jellyfin_recv_ctx_t *)evt->user_data;
        if (ctx->len + (size_t)evt->data_len <= ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
            ctx->len += (size_t)evt->data_len;
        }
    }
    return ESP_OK;
}

static esp_err_t get_json(const char *url, uint8_t *buf, size_t cap, size_t *out_len) {
    jellyfin_recv_ctx_t ctx = {.buf = buf, .len = 0, .cap = cap - 1};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Emby-Token", jellyfin_client_token());

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        return ESP_FAIL;
    }
    ctx.buf[ctx.len] = '\0';
    *out_len = ctx.len;
    return ESP_OK;
}

static bool json_string(cJSON *object, const char *key, char *out, size_t out_size) {
    cJSON *value = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(value) || value->valuestring[0] == '\0') return false;
    strlcpy(out, value->valuestring, out_size);
    return true;
}

static void json_first_artist(cJSON *item, char *out, size_t out_size) {
    cJSON *artists = cJSON_GetObjectItem(item, "Artists");
    cJSON *first = cJSON_IsArray(artists) ? cJSON_GetArrayItem(artists, 0) : NULL;
    if (cJSON_IsString(first) && first->valuestring[0] != '\0') {
        strlcpy(out, first->valuestring, out_size);
        return;
    }
    if (!json_string(item, "AlbumArtist", out, out_size)) strlcpy(out, "Unknown artist", out_size);
}

static uint16_t json_u16(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0) return 0;
    if (value->valuedouble > 65535) return 65535;
    return (uint16_t)value->valuedouble;
}

static esp_err_t append_items(media_library_t *library, cJSON *items) {
    int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;

        char id[64];
        char title[160];
        char artist[160];
        char album[160];
        if (!json_string(item, "Id", id, sizeof(id))) continue;
        if (!json_string(item, "Name", title, sizeof(title))) strlcpy(title, id, sizeof(title));
        json_first_artist(item, artist, sizeof(artist));
        if (!json_string(item, "Album", album, sizeof(album))) strlcpy(album, "Jellyfin", sizeof(album));

        char path[96];
        snprintf(path, sizeof(path), JELLYFIN_PATH_PREFIX "%s", id);
        esp_err_t err = media_library_add_track(library, path, title, artist, album,
                                                json_u16(item, "IndexNumber"),
                                                json_u16(item, "ParentIndexNumber"));
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t jellyfin_client_append_tracks(media_library_t *library) {
    if (!jellyfin_client_configured()) return ESP_OK;
    if (!wifi_setup_connect_blocking(20000)) return ESP_ERR_TIMEOUT;

    uint8_t *buf = malloc(JELLYFIN_HTTP_CAP);
    if (buf == NULL) {
        wifi_setup_disconnect();
        return ESP_ERR_NO_MEM;
    }

    char base[160];
    char url[512];
    base_url(base, sizeof(base));
    ensure_config_loaded();
    if (s_config.user_id[0] != '\0') {
        snprintf(url, sizeof(url),
                 "%s/Users/%s/Items?Recursive=true&IncludeItemTypes=Audio&Fields=Album,Artists,AlbumArtist,IndexNumber,ParentIndexNumber&SortBy=AlbumArtist,Album,SortName",
                 base, s_config.user_id);
    } else {
        snprintf(url, sizeof(url),
                 "%s/Items?Recursive=true&IncludeItemTypes=Audio&Fields=Album,Artists,AlbumArtist,IndexNumber,ParentIndexNumber&SortBy=AlbumArtist,Album,SortName",
                 base);
    }

    size_t len = 0;
    esp_err_t err = get_json(url, buf, JELLYFIN_HTTP_CAP, &len);
    if (err == ESP_OK) {
        cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
        cJSON *items = root ? cJSON_GetObjectItem(root, "Items") : NULL;
        if (cJSON_IsArray(items)) err = append_items(library, items);
        else err = ESP_FAIL;
        if (root) cJSON_Delete(root);
    }
    free(buf);
    wifi_setup_disconnect();
    if (err == ESP_OK) ESP_LOGI(TAG, "Jellyfin tracks appended");
    return err;
}

esp_err_t jellyfin_client_stream_url(const char *path, char *out, size_t out_size) {
    if (!jellyfin_client_is_path(path)) return ESP_ERR_INVALID_ARG;
    char base[160];
    base_url(base, sizeof(base));
    const char *id = path + strlen(JELLYFIN_PATH_PREFIX);
    ensure_config_loaded();
    int written;
    if (s_config.user_id[0] != '\0') {
        written = snprintf(out, out_size,
                           "%s/Audio/%s/universal?UserId=%s&MaxStreamingBitrate=192000&AudioCodec=mp3&Container=mp3&TranscodingContainer=mp3&TranscodingProtocol=http",
                           base, id, s_config.user_id);
    } else {
        written = snprintf(out, out_size,
                           "%s/Audio/%s/universal?MaxStreamingBitrate=192000&AudioCodec=mp3&Container=mp3&TranscodingContainer=mp3&TranscodingProtocol=http",
                           base, id);
    }
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_NO_MEM;
}
