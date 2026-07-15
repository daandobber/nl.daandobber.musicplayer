#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "album_art.h"
#include "app_settings.h"
#include "audio_analysis.h"
#include "audio_player.h"
#include "bsp/audio.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "effects.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "media_library.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#define COLOR_BG       0xff060811
#define COLOR_PANEL    0xff141826
#define COLOR_PANEL_2  0xff202638
#define COLOR_TEXT     0xfff3f5ff
#define COLOR_DIM      0xff9aa3b8
#define COLOR_ACCENT   0xff4de0b5
#define COLOR_SELECTED 0xff274b55
#define COLOR_ERROR    0xffff6b7a
#define NOW_CARD_X      408
#define NOW_CARD_Y      350
#define NOW_CARD_W      376
#define NOW_CARD_H      114

static const char *TAG = "musicplayer";

typedef enum {
    SCREEN_LIBRARY,
    SCREEN_NOW_PLAYING,
    SCREEN_SETTINGS,
    SCREEN_ERROR,
} screen_id_t;

typedef enum {
    LIBRARY_PANE_ARTISTS = 0,
    LIBRARY_PANE_ALBUMS,
    LIBRARY_PANE_QUEUE,
} library_pane_t;

typedef struct {
    size_t *tracks;
    size_t  count;
    size_t  capacity;
    size_t  current;
} play_queue_t;

static size_t display_h_res;
static size_t display_v_res;
static pax_buf_t framebuffers[2];
static unsigned framebuffer_index;
static QueueHandle_t input_queue;
static media_library_t library;
static play_queue_t play_queue;
static size_t selected_artist;
static size_t selected_album;
static size_t selected_queue;
static library_pane_t library_focus = LIBRARY_PANE_ARTISTS;
static screen_id_t current_screen = SCREEN_LIBRARY;
static screen_id_t settings_return_screen = SCREEN_LIBRARY;
static app_settings_t settings;
static size_t selected_setting;
static int64_t last_input_time;
static int64_t last_effect_change_time;
static uint32_t last_effect_beat;
static bool display_dimmed;
static char error_message[160];
static bool render_dirty = true;
static int64_t fps_window_start;
static uint64_t render_time_total;
static uint64_t effect_time_total;
static uint64_t overlay_time_total;
static uint64_t present_time_total;
static uint32_t rendered_frames;
static bool now_card_valid[2];
static bool now_card_visible = true;
static bool overlay_signature_valid;
static audio_player_state_t overlay_state;
static uint32_t overlay_sample_rate;
static uint8_t overlay_channels;
static effect_id_t overlay_effect;
static char overlay_path[AUDIO_PLAYER_PATH_MAX];
static int64_t now_announce_start;

static pax_orientation_t display_orientation(void) {
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90: return PAX_O_ROT_CCW;
        case BSP_DISPLAY_ROTATION_180: return PAX_O_ROT_HALF;
        case BSP_DISPLAY_ROTATION_270: return PAX_O_ROT_CW;
        default: return PAX_O_UPRIGHT;
    }
}

static void present(pax_buf_t *buffer) {
    esp_err_t err = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(buffer));
    if (err != ESP_OK) ESP_LOGE(TAG, "Display blit failed: %s", esp_err_to_name(err));
    framebuffer_index ^= 1;
}

static void clipped_text(char *output, size_t output_size, const char *input, size_t max_chars) {
    if (output_size == 0) return;
    size_t length = strlen(input);
    if (length <= max_chars) {
        strlcpy(output, input, output_size);
        return;
    }
    if (max_chars < 4) {
        output[0] = '\0';
        return;
    }
    size_t keep = max_chars - 3;
    if (keep >= output_size) keep = output_size - 1;
    memcpy(output, input, keep);
    output[keep] = '\0';
    strlcat(output, "...", output_size);
}

static void draw_header(pax_buf_t *buffer, const char *title, const char *right) {
    int width = pax_buf_get_width(buffer);
    pax_simple_rect(buffer, COLOR_PANEL, 0, 0, width, 55);
    pax_draw_text(buffer, COLOR_ACCENT, pax_font_sky_mono, 27, 20, 12, title);
    if (right) {
        pax_vec2f size = pax_text_size(pax_font_sky_mono, 18, right);
        pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 18, width - size.x - 20, 18, right);
    }
}

static esp_err_t queue_reserve(size_t needed) {
    if (needed <= play_queue.capacity) return ESP_OK;
    size_t capacity = play_queue.capacity ? play_queue.capacity : 64;
    while (capacity < needed) capacity *= 2;
    size_t *tracks = realloc(play_queue.tracks, capacity * sizeof(size_t));
    if (!tracks) return ESP_ERR_NO_MEM;
    play_queue.tracks = tracks;
    play_queue.capacity = capacity;
    return ESP_OK;
}

static void queue_clear(void) {
    play_queue.count = 0;
    play_queue.current = SIZE_MAX;
    selected_queue = 0;
}

static esp_err_t queue_append_range(size_t first_track, size_t track_count) {
    ESP_RETURN_ON_ERROR(queue_reserve(play_queue.count + track_count), TAG, "Queue allocation failed");
    for (size_t i = 0; i < track_count; i++) play_queue.tracks[play_queue.count++] = first_track + i;
    return ESP_OK;
}

static esp_err_t queue_replace_album(size_t album_index) {
    if (album_index >= library.album_count) return ESP_ERR_INVALID_ARG;
    queue_clear();
    const media_album_t *album = &library.albums[album_index];
    return queue_append_range(album->first_track, album->track_count);
}

static esp_err_t queue_append_artist(size_t artist_index) {
    if (artist_index >= library.artist_count) return ESP_ERR_INVALID_ARG;
    const media_artist_t *artist = &library.artists[artist_index];
    for (size_t i = 0; i < artist->album_count; i++) {
        const media_album_t *album = &library.albums[artist->first_album + i];
        ESP_RETURN_ON_ERROR(queue_append_range(album->first_track, album->track_count), TAG, "Queue append failed");
    }
    return ESP_OK;
}

static void queue_remove(size_t position) {
    if (position >= play_queue.count) return;
    if (play_queue.current == position) {
        audio_player_stop();
        play_queue.current = SIZE_MAX;
    } else if (play_queue.current != SIZE_MAX && position < play_queue.current) {
        play_queue.current--;
    }
    memmove(&play_queue.tracks[position], &play_queue.tracks[position + 1],
            (play_queue.count - position - 1) * sizeof(size_t));
    play_queue.count--;
    if (selected_queue >= play_queue.count) selected_queue = play_queue.count ? play_queue.count - 1 : 0;
}

static void draw_pane_frame(pax_buf_t *buffer, int x, int y, int width, int height, const char *title, bool focused) {
    pax_simple_rect(buffer, 0xff0d111d, x, y, width, height);
    pax_simple_rect(buffer, focused ? COLOR_ACCENT : 0xff30384c, x, y, width, 2);
    pax_simple_rect(buffer, focused ? COLOR_ACCENT : 0xff30384c, x, y + height - 2, width, 2);
    pax_simple_rect(buffer, focused ? COLOR_ACCENT : 0xff30384c, x, y, 2, height);
    pax_simple_rect(buffer, focused ? COLOR_ACCENT : 0xff30384c, x + width - 2, y, 2, height);
    pax_simple_rect(buffer, COLOR_PANEL_2, x + 2, y + 2, width - 4, 27);
    pax_draw_text(buffer, focused ? COLOR_TEXT : COLOR_DIM, pax_font_sky_mono, 18, x + 9, y + 6, title);
}

static size_t visible_start(size_t selected, size_t count, size_t rows) {
    if (count <= rows || selected < rows) return 0;
    size_t first = selected - rows + 1;
    return first + rows > count ? count - rows : first;
}

static void render_library(pax_buf_t *buffer) {
    now_card_valid[framebuffer_index] = false;
    const int width = pax_buf_get_width(buffer);
    const int height = pax_buf_get_height(buffer);
    const int top = 58;
    const int bottom = 414;
    const int pane_height = bottom - top;
    const int row_height = 25;
    const size_t rows = (pane_height - 35) / row_height;
    const int artists_x = 8, artists_w = 226;
    const int albums_x = 240, albums_w = 252;
    const int queue_x = 498, queue_w = width - queue_x - 8;

    pax_background(buffer, COLOR_BG);
    char summary[80];
    snprintf(summary, sizeof(summary), "%u artists  %u albums  %u tracks", (unsigned)library.artist_count,
             (unsigned)library.album_count, (unsigned)library.count);
    draw_header(buffer, "MUSICPLAYER // LIBRARY", summary);

    if (library.count == 0) {
        pax_draw_text(buffer, COLOR_ERROR, pax_font_sky_mono, 24, 28, 105, "No MP3 or WAV files found.");
        pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 18, 28, 145,
                      "Add music to the SD card and press Return to scan again.");
        return;
    }

    draw_pane_frame(buffer, artists_x, top, artists_w, pane_height, "ARTISTS", library_focus == LIBRARY_PANE_ARTISTS);
    draw_pane_frame(buffer, albums_x, top, albums_w, pane_height, "ALBUMS", library_focus == LIBRARY_PANE_ALBUMS);
    draw_pane_frame(buffer, queue_x, top, queue_w, pane_height, "PLAYLIST", library_focus == LIBRARY_PANE_QUEUE);

    size_t artist_first = visible_start(selected_artist, library.artist_count, rows);
    for (size_t row = 0; row < rows && artist_first + row < library.artist_count; row++) {
        size_t index = artist_first + row;
        int y = top + 34 + row * row_height;
        if (index == selected_artist) pax_simple_rect(buffer, COLOR_SELECTED, artists_x + 4, y - 2, artists_w - 8, row_height);
        char clipped[64];
        clipped_text(clipped, sizeof(clipped), library.artists[index].name, 14);
        pax_draw_text(buffer, index == selected_artist ? COLOR_TEXT : COLOR_DIM, pax_font_sky_mono, 18,
                      artists_x + 9, y, clipped);
    }

    const media_artist_t *artist = &library.artists[selected_artist];
    size_t album_offset = selected_album >= artist->first_album ? selected_album - artist->first_album : 0;
    size_t album_first = visible_start(album_offset, artist->album_count, rows);
    for (size_t row = 0; row < rows && album_first + row < artist->album_count; row++) {
        size_t index = artist->first_album + album_first + row;
        int y = top + 34 + row * row_height;
        if (index == selected_album) pax_simple_rect(buffer, COLOR_SELECTED, albums_x + 4, y - 2, albums_w - 8, row_height);
        char clipped[68];
        clipped_text(clipped, sizeof(clipped), library.albums[index].name, 16);
        pax_draw_text(buffer, index == selected_album ? COLOR_TEXT : COLOR_DIM, pax_font_sky_mono, 18,
                      albums_x + 9, y, clipped);
    }

    size_t queue_first = visible_start(selected_queue, play_queue.count, rows);
    for (size_t row = 0; row < rows && queue_first + row < play_queue.count; row++) {
        size_t position = queue_first + row;
        size_t track_index = play_queue.tracks[position];
        const media_track_t *track = &library.tracks[track_index];
        int y = top + 34 + row * row_height;
        if (position == selected_queue) pax_simple_rect(buffer, COLOR_SELECTED, queue_x + 4, y - 2, queue_w - 8, row_height);
        char title[56], text[72];
        clipped_text(title, sizeof(title), track->title, 16);
        snprintf(text, sizeof(text), "%c%02u %s", position == play_queue.current ? '>' : ' ',
                 track->track_number ? track->track_number : (unsigned)(position + 1), title);
        pax_col_t color = position == play_queue.current ? COLOR_ACCENT :
                          (position == selected_queue ? COLOR_TEXT : COLOR_DIM);
        pax_draw_text(buffer, color, pax_font_sky_mono, 18, queue_x + 9, y, text);
    }

    pax_simple_rect(buffer, COLOR_PANEL, 0, bottom + 5, width, height - bottom - 5);
    audio_player_snapshot_t player;
    audio_player_get_snapshot(&player);
    size_t current_track = media_library_find_path(&library, player.path);
    if (current_track != SIZE_MAX) {
        const media_track_t *track = &library.tracks[current_track];
        char now[120], clipped[105];
        snprintf(now, sizeof(now), "%s  -  %s", track->artist, track->title);
        clipped_text(clipped, sizeof(clipped), now, 70);
        pax_draw_text(buffer, COLOR_ACCENT, pax_font_sky_mono, 18, 16, bottom + 13, clipped);
    } else {
        pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 18, 16, bottom + 13, "No active track");
    }
    pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 9, 16, bottom + 40,
                  "Left/Right pane  Up/Down select  Return load/play  Space append  Backspace remove");
}

static const char *state_name(audio_player_state_t state) {
    switch (state) {
        case AUDIO_PLAYER_PLAYING: return "PLAY";
        case AUDIO_PLAYER_PAUSED: return "PAUSED";
        case AUDIO_PLAYER_ERROR: return "ERROR";
        default: return "STOP";
    }
}

static pax_col_t color_scale(pax_col_t color, float scale) {
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;
    return pax_col_rgb((int)(((color >> 16) & 255) * scale),
                       (int)(((color >> 8) & 255) * scale),
                       (int)((color & 255) * scale));
}

static void draw_now_card(pax_buf_t *buffer, const audio_player_snapshot_t *player) {
    size_t library_track = media_library_find_path(&library, player->path);
    const media_track_t *metadata = library_track != SIZE_MAX ? &library.tracks[library_track] : NULL;
    const char *title_source = metadata ? metadata->title : media_library_display_name(player->path);
    const char *artist_source = metadata ? metadata->artist : "Unknown artist";
    const char *album_source = metadata ? metadata->album : "Unknown album";

    pax_col_t accent = effects_palette_color(.82f, 0.0f);
    pax_col_t secondary = effects_palette_color(.42f, 0.0f);
    pax_simple_rect(buffer, 0xff010207, NOW_CARD_X + 5, NOW_CARD_Y + 5, NOW_CARD_W, NOW_CARD_H);
    pax_simple_rect(buffer, 0xff080a13, NOW_CARD_X, NOW_CARD_Y, NOW_CARD_W, NOW_CARD_H);
    pax_simple_rect(buffer, 0xff0d1020, NOW_CARD_X + 5, NOW_CARD_Y + 5, NOW_CARD_W - 10, NOW_CARD_H - 10);
    pax_simple_rect(buffer, color_scale(secondary, .26f), NOW_CARD_X + 5, NOW_CARD_Y + 5,
                    NOW_CARD_W - 10, 18);
    pax_simple_rect(buffer, accent, NOW_CARD_X, NOW_CARD_Y + 9, 4, NOW_CARD_H - 18);
    pax_simple_rect(buffer, color_scale(accent, .62f), NOW_CARD_X + 4, NOW_CARD_Y + 9, 2,
                    NOW_CARD_H - 18);
    pax_simple_rect(buffer, accent, NOW_CARD_X + 15, NOW_CARD_Y, 52, 2);
    pax_simple_rect(buffer, secondary, NOW_CARD_X + NOW_CARD_W - 74, NOW_CARD_Y + NOW_CARD_H - 2, 58, 2);

    const int art_x = NOW_CARD_X + 13;
    const int art_y = NOW_CARD_Y + 12;
    const int art_size = NOW_CARD_H - 24;
    pax_simple_rect(buffer, color_scale(accent, .65f), art_x - 2, art_y - 2, art_size + 4, art_size + 4);
    if (!metadata || !album_art_draw(buffer, metadata, art_x, art_y, art_size)) {
        pax_simple_rect(buffer, color_scale(secondary, .48f), art_x, art_y, art_size, art_size);
        for (int stripe = 0; stripe < 5; stripe++) {
            int stripe_y = art_y + 8 + stripe * 17;
            pax_simple_rect(buffer, color_scale(stripe & 1 ? accent : secondary, .45f + stripe * .10f),
                            art_x + 7, stripe_y, art_size - 14, 8);
        }
        float disc_x = art_x + art_size * 0.62f;
        float disc_y = art_y + art_size * 0.52f;
        pax_simple_circle(buffer, 0xffe8edf5, disc_x, disc_y, art_size * 0.34f);
        pax_simple_circle(buffer, secondary, disc_x, disc_y, art_size * 0.24f);
        pax_simple_circle(buffer, 0xff080a13, disc_x, disc_y, art_size * 0.065f);
    }

    const int text_x = art_x + art_size + 17;
    char album[52], title[52], artist[52];
    clipped_text(album, sizeof(album), album_source, 28);
    clipped_text(title, sizeof(title), title_source, 20);
    clipped_text(artist, sizeof(artist), artist_source, 22);
    pax_draw_text(buffer, color_scale(secondary, .88f), pax_font_sky_mono, 9,
                  text_x, NOW_CARD_Y + 9, "MILKDRIP // NOW PLAYING");
    pax_draw_text(buffer, COLOR_TEXT, pax_font_sky_mono, 19, text_x, NOW_CARD_Y + 30, title);
    pax_draw_text(buffer, accent, pax_font_sky_mono, 14, text_x, NOW_CARD_Y + 56, artist);
    pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 9, text_x, NOW_CARD_Y + 77, album);

    char detail[64];
    snprintf(detail, sizeof(detail), "%s  //  %s", state_name(player->state), effects_name());
    pax_draw_text(buffer, color_scale(accent, .72f), pax_font_sky_mono, 9,
                  text_x, NOW_CARD_Y + 96, detail);
}

static void draw_now_announce(pax_buf_t *buffer, const audio_player_snapshot_t *player,
                              const audio_analysis_snapshot_t *analysis) {
    float elapsed = (esp_timer_get_time() - now_announce_start) / 1000000.0f;
    if (elapsed < .12f || elapsed > 4.6f) return;

    size_t library_track = media_library_find_path(&library, player->path);
    const media_track_t *metadata = library_track != SIZE_MAX ? &library.tracks[library_track] : NULL;
    const char *title_source = metadata ? metadata->title : media_library_display_name(player->path);
    const char *artist_source = metadata ? metadata->artist : "Unknown artist";
    char title[64], artist[64];
    clipped_text(title, sizeof(title), title_source, 32);
    clipped_text(artist, sizeof(artist), artist_source, 36);

    pax_col_t accent = effects_palette_color(.82f, analysis->beat_strength);
    pax_col_t secondary = effects_palette_color(.38f, analysis->beat_strength * .5f);
    float pulse = .76f + analysis->bass * .24f;
    int center = pax_buf_get_width(buffer) / 2;
    int baseline = 338 + (int)(analysis->bass * 7.0f);
    for (int band = 0; band < 6; band++) {
        float strength = analysis->bands[band * 2];
        int length = 18 + (int)(strength * 88.0f);
        int y = baseline - 31 + band * 10;
        pax_simple_rect(buffer, color_scale(band & 1 ? accent : secondary, .34f),
                        center - 260 - length, y, length, 2);
        pax_simple_rect(buffer, color_scale(band & 1 ? secondary : accent, .34f),
                        center + 260, y, length, 2);
    }

    pax_vec2f title_size = pax_text_size(pax_font_sky_mono, 25, title);
    pax_vec2f artist_size = pax_text_size(pax_font_sky_mono, 15, artist);
    float title_x = center - title_size.x * .5f;
    pax_draw_text(buffer, color_scale(COLOR_TEXT, pulse), pax_font_sky_mono, 25,
                  title_x, baseline, title);
    pax_draw_text(buffer, accent, pax_font_sky_mono, 15,
                  center - artist_size.x * .5f, baseline + 39, artist);
    int line = 54 + (int)(analysis->rms * 170.0f);
    pax_simple_rect(buffer, color_scale(accent, .82f), center - line, baseline - 12,
                    line * 2, 2);
}

static void render_now_playing(pax_buf_t *buffer, float dt) {
    audio_analysis_snapshot_t analysis;
    audio_player_snapshot_t player;
    audio_analysis_get(&analysis);
    audio_player_get_snapshot(&player);
    if (player.state != AUDIO_PLAYER_PLAYING) {
        analysis.beat_strength = 0;
        if (player.state == AUDIO_PLAYER_STOPPED) memset(&analysis, 0, sizeof(analysis));
    }

    int64_t stage_start = esp_timer_get_time();
    effects_render(buffer, &analysis, dt);
    int64_t overlay_start = esp_timer_get_time();
    effect_time_total += overlay_start - stage_start;

    effect_id_t active_effect = effects_current();
    bool track_changed = !overlay_signature_valid || strcmp(overlay_path, player.path) != 0;
    bool overlay_changed = !overlay_signature_valid || overlay_state != player.state ||
                           overlay_sample_rate != player.sample_rate ||
                           overlay_channels != player.channels || overlay_effect != active_effect ||
                           strcmp(overlay_path, player.path) != 0;
    if (overlay_changed) {
        if (track_changed) now_announce_start = esp_timer_get_time();
        overlay_signature_valid = true;
        overlay_state = player.state;
        overlay_sample_rate = player.sample_rate;
        overlay_channels = player.channels;
        overlay_effect = active_effect;
        strlcpy(overlay_path, player.path, sizeof(overlay_path));
        now_card_valid[0] = false;
        now_card_valid[1] = false;
    }

    if (now_card_visible && !now_card_valid[framebuffer_index]) {
        draw_now_card(buffer, &player);
        now_card_valid[framebuffer_index] = true;
    }
    if (now_card_visible) {
        pax_col_t accent = effects_palette_color(.82f, analysis.beat_strength);
        pax_col_t secondary = effects_palette_color(.42f, analysis.beat_strength * .5f);
        int trace_width = 42 + (int)(analysis.bass * 122.0f);
        pax_simple_rect(buffer, 0xff0d1020, NOW_CARD_X + 15, NOW_CARD_Y,
                        170, 2);
        pax_simple_rect(buffer, 0xff0d1020, NOW_CARD_X + NOW_CARD_W - 124,
                        NOW_CARD_Y + 6, 100, 17);
        pax_simple_rect(buffer, accent, NOW_CARD_X + 15, NOW_CARD_Y, trace_width, 2);
        for (int band = 0; band < AUDIO_ANALYSIS_BANDS; band++) {
            int height = 2 + (int)(analysis.bands[band] * 12.0f);
            pax_simple_rect(buffer, band & 1 ? accent : secondary,
                            NOW_CARD_X + NOW_CARD_W - 122 + band * 8,
                            NOW_CARD_Y + 22 - height, 5, height);
        }
        int active_segments = (player.volume + 9) / 10;
        pax_draw_text(buffer, color_scale(secondary, .75f), pax_font_sky_mono, 9,
                      NOW_CARD_X + NOW_CARD_W - 132, NOW_CARD_Y + NOW_CARD_H - 14, "VOL");
        for (int segment = 0; segment < 10; segment++) {
            pax_col_t color = segment < active_segments ? accent : 0xff303746;
            pax_simple_rect(buffer, color, NOW_CARD_X + NOW_CARD_W - 104 + segment * 9,
                            NOW_CARD_Y + NOW_CARD_H - 13, 6, 6);
        }
    } else {
        draw_now_announce(buffer, &player, &analysis);
    }
    if (player.state == AUDIO_PLAYER_ERROR) {
        int width = pax_buf_get_width(buffer);
        int height = pax_buf_get_height(buffer);
        pax_simple_rect(buffer, 0xff60101a, 90, height / 2 - 44, width - 180, 88);
        pax_draw_text(buffer, COLOR_ERROR, pax_font_sky_mono, 20, 110, height / 2 - 25, player.error);
    }
    overlay_time_total += esp_timer_get_time() - overlay_start;
}

#define SETTINGS_ITEM_COUNT 9

static const char *setting_label(size_t index) {
    static const char *labels[SETTINGS_ITEM_COUNT] = {
        "Automatic effects", "Order", "Switch interval", "Beats per effect",
        "Brightness", "Dim after", "Dim level", "Visual intensity", "Test dimming"
    };
    return labels[index];
}

static void setting_value(size_t index, char *value, size_t value_size) {
    static const char *modes[] = {"Off", "Timed", "On beats"};
    static const char *intensities[] = {"Calm", "Normal", "Intense"};
    switch (index) {
        case 0: strlcpy(value, modes[settings.auto_effect_mode], value_size); break;
        case 1: strlcpy(value, settings.shuffle_effects ? "Shuffle" : "Sequential", value_size); break;
        case 2: snprintf(value, value_size, "%u sec", settings.auto_seconds); break;
        case 3: snprintf(value, value_size, "%u beats", settings.auto_beats); break;
        case 4: snprintf(value, value_size, "%u%%", settings.brightness); break;
        case 5:
            if (settings.dim_timeout_seconds == 0) strlcpy(value, "Never", value_size);
            else snprintf(value, value_size, "%u sec", settings.dim_timeout_seconds);
            break;
        case 6: snprintf(value, value_size, "%u%%", settings.dim_brightness); break;
        case 7: strlcpy(value, intensities[settings.visual_intensity], value_size); break;
        case 8: strlcpy(value, "Press Right", value_size); break;
        default: value[0] = '\0'; break;
    }
}

static void render_settings(pax_buf_t *buffer) {
    now_card_valid[framebuffer_index] = false;
    int width = pax_buf_get_width(buffer);
    pax_background(buffer, COLOR_BG);
    draw_header(buffer, "MUSICPLAYER // SETTINGS", "F5");
    for (size_t i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        int y = 70 + i * 39;
        if (i == selected_setting) pax_simple_rect(buffer, COLOR_SELECTED, 42, y - 6, width - 84, 36);
        pax_draw_text(buffer, i == selected_setting ? COLOR_TEXT : COLOR_DIM,
                      pax_font_sky_mono, 18, 58, y, setting_label(i));
        char value[40];
        setting_value(i, value, sizeof(value));
        pax_vec2f size = pax_text_size(pax_font_sky_mono, 18, value);
        pax_draw_text(buffer, i == selected_setting ? COLOR_ACCENT : COLOR_DIM,
                      pax_font_sky_mono, 18, width - 58 - size.x, y, value);
    }
    pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 9, 48, 440,
                  "Up/Down select    Left/Right change    Esc/F5 back    settings save automatically");
}

static void render_error(pax_buf_t *buffer) {
    now_card_valid[framebuffer_index] = false;
    int width = pax_buf_get_width(buffer);
    int height = pax_buf_get_height(buffer);
    pax_background(buffer, COLOR_BG);
    draw_header(buffer, "MUSICPLAYER", "ERROR");
    pax_draw_text(buffer, COLOR_ERROR, pax_font_sky_mono, 25, 35, 115, error_message);
    pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 18, 35, 165,
                  "Return: try again   Esc/F1: return to launcher");
    pax_simple_rect(buffer, COLOR_PANEL_2, 35, height - 110, width - 70, 55);
    pax_draw_text(buffer, COLOR_DIM, pax_font_sky_mono, 16, 55, height - 90,
                  "Check that the SD card uses FAT32/exFAT and is inserted correctly.");
}

static void render_frame(float dt) {
    int64_t render_start = esp_timer_get_time();
    pax_buf_t *buffer = &framebuffers[framebuffer_index];
    switch (current_screen) {
        case SCREEN_LIBRARY: render_library(buffer); break;
        case SCREEN_NOW_PLAYING: render_now_playing(buffer, dt); break;
        case SCREEN_SETTINGS: render_settings(buffer); break;
        case SCREEN_ERROR: render_error(buffer); break;
    }
    int64_t present_start = esp_timer_get_time();
    present(buffer);
    present_time_total += esp_timer_get_time() - present_start;
    render_dirty = false;

    int64_t render_end = esp_timer_get_time();
    render_time_total += render_end - render_start;
    rendered_frames++;
    if (fps_window_start == 0) fps_window_start = render_start;
    int64_t window = render_end - fps_window_start;
    if (window >= 2000000) {
        ESP_LOGI(TAG, "Render [%s]: %.1f FPS, total %.1f ms, effect %.1f, overlay %.1f, present %.1f",
                 effects_name(), rendered_frames * 1000000.0f / window,
                 render_time_total / (rendered_frames * 1000.0f),
                 effect_time_total / (rendered_frames * 1000.0f),
                 overlay_time_total / (rendered_frames * 1000.0f),
                 present_time_total / (rendered_frames * 1000.0f));
        fps_window_start = render_end;
        render_time_total = 0;
        effect_time_total = 0;
        overlay_time_total = 0;
        present_time_total = 0;
        rendered_frames = 0;
    }
}

static esp_err_t reload_library(void) {
    audio_player_stop();
    album_art_invalidate();
    queue_clear();
    media_library_clear(&library);
    media_library_unmount();
    esp_err_t err = media_library_mount();
    if (err == ESP_OK) err = media_library_scan(&library);
    if (err != ESP_OK) {
        snprintf(error_message, sizeof(error_message), "Unable to read the SD card (%s)", esp_err_to_name(err));
        current_screen = SCREEN_ERROR;
        return err;
    }
    selected_artist = 0;
    selected_album = library.artist_count ? library.artists[0].first_album : 0;
    selected_queue = 0;
    library_focus = LIBRARY_PANE_ARTISTS;
    if (library.count && queue_append_range(0, library.count) != ESP_OK) {
        strlcpy(error_message, "Onvoldoende geheugen voor playlist", sizeof(error_message));
        current_screen = SCREEN_ERROR;
        return ESP_ERR_NO_MEM;
    }
    current_screen = SCREEN_LIBRARY;
    render_dirty = true;
    return ESP_OK;
}

static void play_queue_position(size_t position) {
    if (position >= play_queue.count) return;
    size_t track_index = play_queue.tracks[position];
    if (track_index >= library.count) return;
    if (audio_player_play(library.tracks[track_index].path) == ESP_OK) {
        play_queue.current = position;
        selected_queue = position;
        current_screen = SCREEN_NOW_PLAYING;
        now_card_valid[0] = false;
        now_card_valid[1] = false;
        overlay_signature_valid = false;
        render_dirty = true;
    }
}

static void play_relative(int direction) {
    if (play_queue.count == 0) return;
    size_t position = play_queue.current == SIZE_MAX ? selected_queue : play_queue.current;
    if (position >= play_queue.count) position = 0;
    if (direction > 0) {
        position = (position + 1) % play_queue.count;
    } else {
        position = (position + play_queue.count - 1) % play_queue.count;
    }
    play_queue_position(position);
}

static void select_artist_relative(int delta) {
    if (!library.artist_count) return;
    selected_artist = delta > 0 ? (selected_artist + 1) % library.artist_count
                                : (selected_artist + library.artist_count - 1) % library.artist_count;
    selected_album = library.artists[selected_artist].first_album;
}

static void select_album_relative(int delta) {
    if (!library.artist_count) return;
    const media_artist_t *artist = &library.artists[selected_artist];
    if (!artist->album_count) return;
    size_t offset = selected_album - artist->first_album;
    offset = delta > 0 ? (offset + 1) % artist->album_count : (offset + artist->album_count - 1) % artist->album_count;
    selected_album = artist->first_album + offset;
}

static void select_queue_relative(int delta) {
    if (!play_queue.count) return;
    selected_queue = delta > 0 ? (selected_queue + 1) % play_queue.count
                               : (selected_queue + play_queue.count - 1) % play_queue.count;
}

static bool is_space_key(bsp_input_navigation_key_t key) {
    return key == BSP_INPUT_NAVIGATION_KEY_SPACE_L || key == BSP_INPUT_NAVIGATION_KEY_SPACE_M ||
           key == BSP_INPUT_NAVIGATION_KEY_SPACE_R;
}

static void replace_queue_with_artist(size_t artist_index) {
    queue_clear();
    if (queue_append_artist(artist_index) == ESP_OK && play_queue.count) play_queue_position(0);
}

static void activate_library_selection(void) {
    if (!library.count) {
        reload_library();
    } else if (library_focus == LIBRARY_PANE_ARTISTS) {
        replace_queue_with_artist(selected_artist);
    } else if (library_focus == LIBRARY_PANE_ALBUMS) {
        if (queue_replace_album(selected_album) == ESP_OK) play_queue_position(0);
    } else {
        play_queue_position(selected_queue);
    }
}

static void append_library_selection(void) {
    size_t old_count = play_queue.count;
    esp_err_t err = ESP_OK;
    if (library_focus == LIBRARY_PANE_ARTISTS) {
        err = queue_append_artist(selected_artist);
    } else if (library_focus == LIBRARY_PANE_ALBUMS && selected_album < library.album_count) {
        const media_album_t *album = &library.albums[selected_album];
        err = queue_append_range(album->first_track, album->track_count);
    } else if (library_focus == LIBRARY_PANE_QUEUE) {
        play_queue_position(selected_queue);
        return;
    }
    if (err == ESP_OK && play_queue.count > old_count) selected_queue = old_count;
}

static void adjust_volume(int delta) {
    audio_player_snapshot_t player;
    audio_player_get_snapshot(&player);
    int volume = player.volume + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    audio_player_set_volume((uint8_t)volume);
    render_dirty = true;
}

static uint16_t cycle_choice(uint16_t current, const uint16_t *choices, size_t count, int delta) {
    size_t index = 0;
    while (index + 1 < count && choices[index] != current) index++;
    index = delta > 0 ? (index + 1) % count : (index + count - 1) % count;
    return choices[index];
}

static void apply_settings(void) {
    effects_set_intensity(settings.visual_intensity);
    if (!display_dimmed) bsp_display_set_backlight_brightness(settings.brightness);
    app_settings_save(&settings);
    last_effect_change_time = esp_timer_get_time();
    audio_analysis_snapshot_t analysis;
    audio_analysis_get(&analysis);
    last_effect_beat = analysis.beat_counter;
}

static void change_setting(int delta) {
    static const uint16_t seconds[] = {15, 30, 45, 60, 90, 120, 180};
    static const uint16_t beats[] = {8, 16, 32, 64, 96, 128};
    static const uint16_t dim_times[] = {0, 30, 60, 120, 300, 600};
    switch (selected_setting) {
        case 0: settings.auto_effect_mode = (settings.auto_effect_mode + (delta > 0 ? 1 : 2)) % 3; break;
        case 1: settings.shuffle_effects = !settings.shuffle_effects; break;
        case 2: settings.auto_seconds = cycle_choice(settings.auto_seconds, seconds, sizeof(seconds) / sizeof(seconds[0]), delta); break;
        case 3: settings.auto_beats = cycle_choice(settings.auto_beats, beats, sizeof(beats) / sizeof(beats[0]), delta); break;
        case 4: {
            int value = settings.brightness + delta * 10;
            settings.brightness = value < 20 ? 100 : (value > 100 ? 20 : value);
            break;
        }
        case 5: settings.dim_timeout_seconds = cycle_choice(settings.dim_timeout_seconds, dim_times, sizeof(dim_times) / sizeof(dim_times[0]), delta); break;
        case 6: {
            int value = settings.dim_brightness + delta * 10;
            settings.dim_brightness = value < 0 ? 60 : (value > 60 ? 0 : value);
            break;
        }
        case 7: settings.visual_intensity = (settings.visual_intensity + (delta > 0 ? 1 : 2)) % 3; break;
        case 8:
            display_dimmed = true;
            ESP_LOGI(TAG, "Dim test: %u%%", settings.dim_brightness);
            ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_set_backlight_brightness(settings.dim_brightness));
            break;
        default: break;
    }
    apply_settings();
    render_dirty = true;
}

static void open_settings(void) {
    if (current_screen != SCREEN_SETTINGS) settings_return_screen = current_screen;
    current_screen = SCREEN_SETTINGS;
    render_dirty = true;
}

static void close_settings(void) {
    current_screen = settings_return_screen;
    now_card_valid[0] = false;
    now_card_valid[1] = false;
    render_dirty = true;
}

static void reset_effect_automation(void) {
    last_effect_change_time = esp_timer_get_time();
    audio_analysis_snapshot_t analysis;
    audio_analysis_get(&analysis);
    last_effect_beat = analysis.beat_counter;
}

static void handle_navigation(const bsp_input_event_args_navigation_t *navigation) {
    if (!navigation->state) return;
    bsp_input_navigation_key_t key = navigation->key;

    if (key == BSP_INPUT_NAVIGATION_KEY_F1) {
        audio_player_stop();
        bsp_audio_set_amplifier(false);
        bsp_device_restart_to_launcher();
        return;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP) {
        adjust_volume(5);
        return;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) {
        adjust_volume(-5);
        return;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_F5) {
        if (current_screen == SCREEN_SETTINGS) close_settings();
        else open_settings();
        return;
    }

    if (current_screen == SCREEN_SETTINGS) {
        if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
            selected_setting = (selected_setting + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
        } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            selected_setting = (selected_setting + 1) % SETTINGS_ITEM_COUNT;
        } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
            change_setting(-1);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT || key == BSP_INPUT_NAVIGATION_KEY_RETURN || is_space_key(key)) {
            change_setting(1);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
            close_settings();
        }
        render_dirty = true;
        return;
    }

    if (current_screen == SCREEN_LIBRARY) {
        if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
            library_focus = (library_pane_t)((library_focus + 2) % 3);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT || key == BSP_INPUT_NAVIGATION_KEY_TAB) {
            library_focus = (library_pane_t)((library_focus + 1) % 3);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_UP || key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            int delta = key == BSP_INPUT_NAVIGATION_KEY_DOWN ? 1 : -1;
            if (library_focus == LIBRARY_PANE_ARTISTS) select_artist_relative(delta);
            else if (library_focus == LIBRARY_PANE_ALBUMS) select_album_relative(delta);
            else select_queue_relative(delta);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_PGUP || key == BSP_INPUT_NAVIGATION_KEY_PGDN) {
            int delta = key == BSP_INPUT_NAVIGATION_KEY_PGDN ? 1 : -1;
            for (int i = 0; i < 8; i++) {
                if (library_focus == LIBRARY_PANE_ARTISTS) select_artist_relative(delta);
                else if (library_focus == LIBRARY_PANE_ALBUMS) select_album_relative(delta);
                else select_queue_relative(delta);
            }
        } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
            activate_library_selection();
        } else if (is_space_key(key)) {
            append_library_selection();
        } else if (key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE && library_focus == LIBRARY_PANE_QUEUE) {
            queue_remove(selected_queue);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC && play_queue.current != SIZE_MAX) {
            current_screen = SCREEN_NOW_PLAYING;
        }
        render_dirty = true;
    } else if (current_screen == SCREEN_NOW_PLAYING) {
        if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
            play_relative(-1);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
            play_relative(1);
        } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN || is_space_key(key)) {
            audio_player_toggle_pause();
        } else if (key == BSP_INPUT_NAVIGATION_KEY_F2 || key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            effects_previous();
            reset_effect_automation();
        } else if (key == BSP_INPUT_NAVIGATION_KEY_F3 || key == BSP_INPUT_NAVIGATION_KEY_UP) {
            effects_next();
            reset_effect_automation();
        } else if (key == BSP_INPUT_NAVIGATION_KEY_F4 || key == BSP_INPUT_NAVIGATION_KEY_MENU) {
            now_card_visible = !now_card_visible;
            effects_set_overlay_visible(now_card_visible);
            if (!now_card_visible) now_announce_start = esp_timer_get_time();
            now_card_valid[0] = false;
            now_card_valid[1] = false;
        } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
            current_screen = SCREEN_LIBRARY;
        }
        render_dirty = true;
    } else if (current_screen == SCREEN_ERROR) {
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
            reload_library();
        } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
            bsp_device_restart_to_launcher();
        }
        render_dirty = true;
    }
}

static void handle_input(const bsp_input_event_t *event) {
    bool user_activity = event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state;
    if (user_activity) {
        last_input_time = esp_timer_get_time();
        if (display_dimmed) {
            display_dimmed = false;
            ESP_LOGI(TAG, "Display wake: %u%%", settings.brightness);
            ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_set_backlight_brightness(settings.brightness));
        }
    }
    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION: handle_navigation(&event->args_navigation); break;
        case INPUT_EVENT_TYPE_ACTION:
            if (event->args_action.type == BSP_INPUT_ACTION_TYPE_SD_CARD) {
                if (event->args_action.state) {
                    reload_library();
                } else {
                    audio_player_stop();
                    queue_clear();
                    media_library_clear(&library);
                    media_library_unmount();
                    strlcpy(error_message, "SD card removed", sizeof(error_message));
                    current_screen = SCREEN_ERROR;
                    render_dirty = true;
                }
            }
            break;
        default: break;
    }
}

static void change_effect_automatically(void) {
    effect_id_t current = effects_current();
    if (settings.shuffle_effects && EFFECT_COUNT > 1) {
        effect_id_t next;
        // The procedural bank is curated for strong visual differences. Keep
        // automatic shuffle there; the older feedback/legacy bank remains
        // reachable manually with F2/F3.
        size_t procedural_count = EFFECT_COUNT - EFFECT_PROCEDURAL_FIRST;
        do {
            next = (effect_id_t)(EFFECT_PROCEDURAL_FIRST + esp_random() % procedural_count);
        } while (next == current);
        effects_select(next);
    } else {
        effects_next();
    }
    now_card_valid[0] = false;
    now_card_valid[1] = false;
    reset_effect_automation();
    render_dirty = true;
}

static void update_timers(int64_t now, const audio_player_snapshot_t *player) {
    if (!display_dimmed && settings.dim_timeout_seconds &&
        now - last_input_time >= (int64_t)settings.dim_timeout_seconds * 1000000) {
        display_dimmed = true;
        ESP_LOGI(TAG, "Display idle dim after %u sec: %u%%", settings.dim_timeout_seconds,
                 settings.dim_brightness);
        ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_set_backlight_brightness(settings.dim_brightness));
    }
    if (current_screen != SCREEN_NOW_PLAYING || player->state != AUDIO_PLAYER_PLAYING ||
        settings.auto_effect_mode == AUTO_EFFECT_OFF) return;
    if (settings.auto_effect_mode == AUTO_EFFECT_TIME &&
        now - last_effect_change_time >= (int64_t)settings.auto_seconds * 1000000) {
        change_effect_automatically();
    } else if (settings.auto_effect_mode == AUTO_EFFECT_BEATS) {
        audio_analysis_snapshot_t analysis;
        audio_analysis_get(&analysis);
        if ((uint32_t)(analysis.beat_counter - last_effect_beat) >= settings.auto_beats) change_effect_automatically();
    }
}

static esp_err_t initialize_display_and_bsp(void) {
    bsp_configuration_t configuration = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
            .num_fbs = 2,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_device_initialize(&configuration), TAG, "BSP init failed");

    bsp_display_color_format_t color_format;
    bsp_display_endianness_t endianness;
    ESP_RETURN_ON_ERROR(bsp_display_get_parameters(&display_h_res, &display_v_res, &color_format, &endianness), TAG,
                        "Display parameters failed");
    if (color_format != BSP_DISPLAY_COLOR_FORMAT_16_565RGB) return ESP_ERR_NOT_SUPPORTED;

    esp_lcd_panel_handle_t panel;
    void *display_pixels[2] = {0};
    ESP_RETURN_ON_ERROR(bsp_display_get_panel(&panel), TAG, "Display panel handle failed");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &display_pixels[0], &display_pixels[1]), TAG,
                        "Display framebuffer access failed");

    pax_orientation_t orientation = display_orientation();
    for (size_t i = 0; i < 2; i++) {
        pax_buf_init(&framebuffers[i], display_pixels[i], display_h_res, display_v_res, PAX_BUF_16_565RGB);
        if (pax_buf_get_pixels(&framebuffers[i]) == NULL) return ESP_ERR_NO_MEM;
        pax_buf_reversed(&framebuffers[i], endianness == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&framebuffers[i], orientation);
    }
    effects_init(pax_buf_get_width(&framebuffers[0]), pax_buf_get_height(&framebuffers[0]));
    return bsp_input_get_queue(&input_queue);
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(initialize_display_and_bsp());
    app_settings_load(&settings);
    effects_set_intensity(settings.visual_intensity);
    bsp_display_set_backlight_brightness(settings.brightness);
    last_input_time = esp_timer_get_time();
    last_effect_change_time = last_input_time;

    err = audio_player_init();
    if (err != ESP_OK) {
        snprintf(error_message, sizeof(error_message), "Audio initialization failed (%s)", esp_err_to_name(err));
        current_screen = SCREEN_ERROR;
    } else {
        reload_library();
    }

    int64_t previous_frame = esp_timer_get_time();
    while (true) {
        bsp_input_event_t event;
        TickType_t input_wait = current_screen == SCREEN_NOW_PLAYING ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(25);
        if (xQueueReceive(input_queue, &event, input_wait) == pdTRUE) {
            handle_input(&event);
            while (xQueueReceive(input_queue, &event, 0) == pdTRUE) handle_input(&event);
        }

        audio_player_event_t player_event;
        while (audio_player_poll_event(&player_event)) {
            if (player_event.type == AUDIO_PLAYER_EVENT_FINISHED &&
                (current_screen == SCREEN_NOW_PLAYING || current_screen == SCREEN_LIBRARY)) {
                screen_id_t previous_screen = current_screen;
                play_relative(1);
                if (previous_screen == SCREEN_LIBRARY) current_screen = SCREEN_LIBRARY;
            } else if (player_event.type == AUDIO_PLAYER_EVENT_ERROR) {
                strlcpy(error_message, player_event.message, sizeof(error_message));
                render_dirty = true;
            }
        }

        audio_player_snapshot_t player;
        audio_player_get_snapshot(&player);
        int64_t now = esp_timer_get_time();
        update_timers(now, &player);
        bool animate = current_screen == SCREEN_NOW_PLAYING && player.state == AUDIO_PLAYER_PLAYING;
        if (animate && now - previous_frame >= 16667) render_dirty = true;  // 60 FPS target.
        if (render_dirty) {
            float dt = (now - previous_frame) / 1000000.0f;
            if (dt <= 0.0f) dt = 1.0f / 30.0f;
            if (dt > 0.25f) dt = 0.25f;
            render_frame(dt);
            previous_frame = now;
        }
    }
}
