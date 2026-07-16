#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char    *path;
    char    *title;
    char    *artist;
    char    *album;
    char    *cover_path;
    uint32_t cover_offset;
    uint32_t cover_size;
    uint16_t track_number;
    uint16_t disc_number;
} media_track_t;

typedef struct {
    const char *name;
    size_t      first_album;
    size_t      album_count;
    size_t      track_count;
} media_artist_t;

typedef struct {
    const char *name;
    size_t      artist_index;
    size_t      first_track;
    size_t      track_count;
} media_album_t;

typedef struct {
    media_track_t  *tracks;
    size_t          count;
    size_t          capacity;
    media_artist_t *artists;
    size_t          artist_count;
    media_album_t  *albums;
    size_t          album_count;
} media_library_t;

esp_err_t media_library_mount(void);
void      media_library_unmount(void);
esp_err_t media_library_scan(media_library_t *library);
esp_err_t media_library_add_track(media_library_t *library, const char *path, const char *title,
                                  const char *artist, const char *album, uint16_t track_number,
                                  uint16_t disc_number);
esp_err_t media_library_rebuild_indexes(media_library_t *library);
void      media_library_clear(media_library_t *library);
size_t    media_library_find_path(const media_library_t *library, const char *path);
const char *media_library_display_name(const char *path);
