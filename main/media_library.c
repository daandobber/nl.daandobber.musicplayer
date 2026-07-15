#include "media_library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define MOUNT_POINT      "/sd"
#define MAX_SCAN_DEPTH   8
#define MAX_MEDIA_FILES  1024
#define META_TEXT_SIZE   160

static const char *TAG = "media";
static sdmmc_card_t *mounted_card;

typedef struct {
    char     title[META_TEXT_SIZE];
    char     artist[META_TEXT_SIZE];
    char     album[META_TEXT_SIZE];
    uint16_t track;
    uint16_t disc;
    uint32_t cover_offset;
    uint32_t cover_size;
} parsed_metadata_t;

static bool is_media_file(const char *name) {
    const char *extension = strrchr(name, '.');
    return extension != NULL &&
           (strcasecmp(extension, ".mp3") == 0 || strcasecmp(extension, ".wav") == 0);
}

static void trim(char *text) {
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
}

static void strip_year_suffix(char *text) {
    size_t length = strlen(text);
    if (length < 7 || text[length - 1] != ')') return;
    char *open = strrchr(text, '(');
    if (open == NULL || strlen(open) != 6) return;
    for (int i = 1; i <= 4; i++) {
        if (!isdigit((unsigned char)open[i])) return;
    }
    while (open > text && isspace((unsigned char)open[-1])) open--;
    *open = '\0';
}

static uint16_t parse_number(const char *text) {
    while (*text && !isdigit((unsigned char)*text)) text++;
    unsigned long value = strtoul(text, NULL, 10);
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static void copy_component(char *out, size_t out_size, const char *start, size_t length) {
    if (out_size == 0) return;
    if (length >= out_size) length = out_size - 1;
    memcpy(out, start, length);
    out[length] = '\0';
    trim(out);
}

static void metadata_from_path(const char *path, parsed_metadata_t *metadata) {
    const char *filename = media_library_display_name(path);
    strlcpy(metadata->title, filename, sizeof(metadata->title));
    char *extension = strrchr(metadata->title, '.');
    if (extension) *extension = '\0';

    char parent[META_TEXT_SIZE] = "Unknown album";
    const char *last_slash = strrchr(path, '/');
    if (last_slash && last_slash > path) {
        const char *parent_end = last_slash;
        const char *parent_start = parent_end;
        while (parent_start > path && parent_start[-1] != '/') parent_start--;
        copy_component(parent, sizeof(parent), parent_start, (size_t)(parent_end - parent_start));
    }
    strip_year_suffix(parent);
    strlcpy(metadata->album, parent, sizeof(metadata->album));
    strlcpy(metadata->artist, "Unknown artist", sizeof(metadata->artist));

    char *separator = strstr(parent, " - ");
    if (separator) {
        copy_component(metadata->artist, sizeof(metadata->artist), parent, (size_t)(separator - parent));
        strlcpy(metadata->album, separator + 3, sizeof(metadata->album));
    }

    // Common rip naming: Artist - Album - 08 - Title (Year)
    const char *cursor = metadata->title;
    const char *number_separator = NULL;
    while ((cursor = strstr(cursor, " - ")) != NULL) {
        const char *candidate = cursor + 3;
        if (isdigit((unsigned char)*candidate)) {
            char *end = NULL;
            unsigned long number = strtoul(candidate, &end, 10);
            if (end && strncmp(end, " - ", 3) == 0) {
                metadata->track = number > UINT16_MAX ? UINT16_MAX : (uint16_t)number;
                number_separator = end + 3;
            }
        }
        cursor += 3;
    }
    if (number_separator) strlcpy(metadata->title, number_separator, sizeof(metadata->title));
    strip_year_suffix(metadata->title);
    trim(metadata->title);
}

static uint32_t read_be32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) | ((uint32_t)value[2] << 8) | value[3];
}

static uint32_t read_le32(const uint8_t *value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint32_t read_syncsafe(const uint8_t *value) {
    return ((uint32_t)(value[0] & 0x7f) << 21) | ((uint32_t)(value[1] & 0x7f) << 14) |
           ((uint32_t)(value[2] & 0x7f) << 7) | (value[3] & 0x7f);
}

static void append_utf8(char *out, size_t out_size, size_t *position, uint32_t codepoint) {
    uint8_t bytes[3];
    size_t count;
    if (codepoint < 0x80) {
        bytes[0] = codepoint;
        count = 1;
    } else if (codepoint < 0x800) {
        bytes[0] = 0xc0 | (codepoint >> 6);
        bytes[1] = 0x80 | (codepoint & 0x3f);
        count = 2;
    } else {
        bytes[0] = 0xe0 | (codepoint >> 12);
        bytes[1] = 0x80 | ((codepoint >> 6) & 0x3f);
        bytes[2] = 0x80 | (codepoint & 0x3f);
        count = 3;
    }
    if (*position + count >= out_size) return;
    memcpy(out + *position, bytes, count);
    *position += count;
}

static void decode_text(const uint8_t *data, size_t length, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (length < 2) return;
    uint8_t encoding = data[0];
    data++;
    length--;
    size_t position = 0;
    if (encoding == 1 || encoding == 2) {
        bool little_endian = false;
        if (length >= 2 && data[0] == 0xff && data[1] == 0xfe) {
            little_endian = true;
            data += 2;
            length -= 2;
        } else if (length >= 2 && data[0] == 0xfe && data[1] == 0xff) {
            data += 2;
            length -= 2;
        }
        for (size_t i = 0; i + 1 < length; i += 2) {
            uint16_t codepoint = little_endian ? (uint16_t)(data[i] | (data[i + 1] << 8))
                                               : (uint16_t)((data[i] << 8) | data[i + 1]);
            if (codepoint == 0) break;
            append_utf8(out, out_size, &position, codepoint);
        }
    } else {
        for (size_t i = 0; i < length && data[i] != 0 && position + 1 < out_size; i++) {
            uint8_t value = data[i];
            if (encoding == 0 && value >= 0x80) {
                append_utf8(out, out_size, &position, value);
            } else {
                out[position++] = (char)value;
            }
        }
    }
    out[position] = '\0';
    trim(out);
}

static void apply_text_frame(const char *id, const uint8_t *data, size_t length, parsed_metadata_t *metadata) {
    char decoded[META_TEXT_SIZE];
    decode_text(data, length, decoded, sizeof(decoded));
    if (decoded[0] == '\0') return;
    if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0) strlcpy(metadata->title, decoded, sizeof(metadata->title));
    else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) strlcpy(metadata->artist, decoded, sizeof(metadata->artist));
    else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0) strlcpy(metadata->album, decoded, sizeof(metadata->album));
    else if (strcmp(id, "TRCK") == 0 || strcmp(id, "TRK") == 0) metadata->track = parse_number(decoded);
    else if (strcmp(id, "TPOS") == 0 || strcmp(id, "TPA") == 0) metadata->disc = parse_number(decoded);
}

static void apply_picture_frame(const uint8_t *data, size_t length, long frame_offset,
                                uint32_t frame_size, parsed_metadata_t *metadata) {
    if (metadata->cover_size || frame_offset < 0) return;
    // APIC/PIC headers can contain differently encoded descriptions. The JPEG
    // SOI marker is the reliable boundary and is always near the frame start.
    for (size_t i = 4; i + 1 < length; i++) {
        if (data[i] == 0xff && data[i + 1] == 0xd8) {
            metadata->cover_offset = (uint32_t)frame_offset + (uint32_t)i;
            metadata->cover_size = frame_size - (uint32_t)i;
            return;
        }
    }
}

static void parse_id3v2(FILE *file, parsed_metadata_t *metadata) {
    uint8_t header[10];
    rewind(file);
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3) != 0) return;
    uint8_t version = header[3];
    if (version < 2 || version > 4) return;
    uint32_t remaining = read_syncsafe(header + 6);
    while (remaining >= (version == 2 ? 6u : 10u)) {
        uint8_t frame_header[10] = {0};
        size_t header_size = version == 2 ? 6 : 10;
        if (fread(frame_header, 1, header_size, file) != header_size) break;
        remaining -= header_size;
        if (frame_header[0] == 0) break;
        char id[5] = {0};
        memcpy(id, frame_header, version == 2 ? 3 : 4);
        uint32_t frame_size = version == 2 ? ((uint32_t)frame_header[3] << 16) |
                                                ((uint32_t)frame_header[4] << 8) | frame_header[5]
                                          : (version == 4 ? read_syncsafe(frame_header + 4)
                                                          : read_be32(frame_header + 4));
        if (frame_size == 0 || frame_size > remaining) break;
        long frame_offset = ftell(file);
        if (id[0] == 'T' && frame_size <= 4096) {
            uint8_t *contents = malloc(frame_size);
            if (contents == NULL) break;
            if (fread(contents, 1, frame_size, file) == frame_size) apply_text_frame(id, contents, frame_size, metadata);
            free(contents);
        } else if (strcmp(id, "APIC") == 0 || strcmp(id, "PIC") == 0) {
            size_t probe_size = frame_size < 1024 ? frame_size : 1024;
            uint8_t *probe = malloc(probe_size);
            if (probe == NULL) break;
            if (fread(probe, 1, probe_size, file) == probe_size) {
                apply_picture_frame(probe, probe_size, frame_offset, frame_size, metadata);
            }
            free(probe);
            fseek(file, frame_offset + frame_size, SEEK_SET);
        } else {
            fseek(file, frame_size, SEEK_CUR);
        }
        remaining -= frame_size;
    }
}

static void copy_id3v1(char *out, size_t out_size, const uint8_t *source, size_t length) {
    while (length && (source[length - 1] == 0 || source[length - 1] == ' ')) length--;
    copy_component(out, out_size, (const char *)source, length);
}

static void parse_id3v1(FILE *file, parsed_metadata_t *metadata) {
    uint8_t tag[128];
    if (fseek(file, -128, SEEK_END) != 0 || fread(tag, 1, sizeof(tag), file) != sizeof(tag) ||
        memcmp(tag, "TAG", 3) != 0) return;
    char text[META_TEXT_SIZE];
    copy_id3v1(text, sizeof(text), tag + 3, 30);
    if (text[0]) strlcpy(metadata->title, text, sizeof(metadata->title));
    copy_id3v1(text, sizeof(text), tag + 33, 30);
    if (text[0]) strlcpy(metadata->artist, text, sizeof(metadata->artist));
    copy_id3v1(text, sizeof(text), tag + 63, 30);
    if (text[0]) strlcpy(metadata->album, text, sizeof(metadata->album));
    if (tag[125] == 0 && tag[126]) metadata->track = tag[126];
}

static void parse_wav_info(FILE *file, parsed_metadata_t *metadata) {
    uint8_t header[12];
    rewind(file);
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) return;
    for (int chunks = 0; chunks < 128; chunks++) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        uint32_t size = read_le32(chunk + 4);
        long data_start = ftell(file);
        if (memcmp(chunk, "LIST", 4) == 0 && size >= 4) {
            uint8_t list_type[4];
            if (fread(list_type, 1, 4, file) == 4 && memcmp(list_type, "INFO", 4) == 0) {
                uint32_t consumed = 4;
                while (consumed + 8 <= size) {
                    uint8_t info_header[8];
                    if (fread(info_header, 1, 8, file) != 8) break;
                    uint32_t info_size = read_le32(info_header + 4);
                    consumed += 8;
                    char text[META_TEXT_SIZE] = {0};
                    size_t take = info_size < sizeof(text) - 1 ? info_size : sizeof(text) - 1;
                    if (take) fread(text, 1, take, file);
                    trim(text);
                    if (memcmp(info_header, "INAM", 4) == 0 && text[0]) strlcpy(metadata->title, text, sizeof(metadata->title));
                    else if (memcmp(info_header, "IART", 4) == 0 && text[0]) strlcpy(metadata->artist, text, sizeof(metadata->artist));
                    else if ((memcmp(info_header, "IPRD", 4) == 0 || memcmp(info_header, "IALB", 4) == 0) && text[0]) strlcpy(metadata->album, text, sizeof(metadata->album));
                    else if (memcmp(info_header, "ITRK", 4) == 0 && text[0]) metadata->track = parse_number(text);
                    fseek(file, data_start + consumed + info_size + (info_size & 1), SEEK_SET);
                    consumed += info_size + (info_size & 1);
                }
            }
        }
        fseek(file, data_start + size + (size & 1), SEEK_SET);
    }
}

static char *find_folder_cover(const char *track_path, uint32_t *cover_size) {
    static const char *names[] = {
        "cover.jpg", "folder.jpg", "front.jpg", "Cover.jpg", "Folder.jpg",
        "cover.jpeg", "folder.jpeg"
    };
    const char *slash = strrchr(track_path, '/');
    if (!slash) return NULL;
    size_t directory_length = (size_t)(slash - track_path);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t length = directory_length + 1 + strlen(names[i]) + 1;
        char *candidate = malloc(length);
        if (!candidate) return NULL;
        snprintf(candidate, length, "%.*s/%s", (int)directory_length, track_path, names[i]);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= UINT32_MAX) {
            *cover_size = (uint32_t)st.st_size;
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

static esp_err_t append_track(media_library_t *library, const char *path) {
    if (library->count >= MAX_MEDIA_FILES) return ESP_ERR_NO_MEM;
    if (library->count == library->capacity) {
        size_t new_capacity = library->capacity == 0 ? 64 : library->capacity * 2;
        if (new_capacity > MAX_MEDIA_FILES) new_capacity = MAX_MEDIA_FILES;
        media_track_t *new_tracks = realloc(library->tracks, new_capacity * sizeof(media_track_t));
        if (new_tracks == NULL) return ESP_ERR_NO_MEM;
        library->tracks = new_tracks;
        library->capacity = new_capacity;
    }

    parsed_metadata_t metadata = {0};
    metadata_from_path(path, &metadata);
    FILE *file = fopen(path, "rb");
    if (file) {
        const char *extension = strrchr(path, '.');
        if (extension && strcasecmp(extension, ".mp3") == 0) {
            parse_id3v1(file, &metadata);
            parse_id3v2(file, &metadata);
        } else {
            parse_wav_info(file, &metadata);
        }
        fclose(file);
    }
    if (metadata.title[0] == '\0') strlcpy(metadata.title, media_library_display_name(path), sizeof(metadata.title));
    if (metadata.artist[0] == '\0') strlcpy(metadata.artist, "Unknown artist", sizeof(metadata.artist));
    if (metadata.album[0] == '\0') strlcpy(metadata.album, "Unknown album", sizeof(metadata.album));

    uint32_t cover_size = 0;
    char *cover_path = find_folder_cover(path, &cover_size);
    uint32_t cover_offset = 0;
    if (!cover_path && metadata.cover_size) {
        cover_path = strdup(path);
        cover_offset = metadata.cover_offset;
        cover_size = metadata.cover_size;
    }

    media_track_t track = {
        .path = strdup(path),
        .title = strdup(metadata.title),
        .artist = strdup(metadata.artist),
        .album = strdup(metadata.album),
        .cover_path = cover_path,
        .cover_offset = cover_offset,
        .cover_size = cover_size,
        .track_number = metadata.track,
        .disc_number = metadata.disc,
    };
    if (!track.path || !track.title || !track.artist || !track.album) {
        free(track.path);
        free(track.title);
        free(track.artist);
        free(track.album);
        free(track.cover_path);
        return ESP_ERR_NO_MEM;
    }
    library->tracks[library->count++] = track;
    return ESP_OK;
}

static esp_err_t scan_directory(media_library_t *library, const char *directory, unsigned depth) {
    if (depth > MAX_SCAN_DEPTH) return ESP_OK;
    DIR *dir = opendir(directory);
    if (dir == NULL) return ESP_FAIL;
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, "System Volume Information") == 0) continue;
        size_t length = strlen(directory) + strlen(entry->d_name) + 2;
        char *path = malloc(length);
        if (path == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        snprintf(path, length, "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) result = scan_directory(library, path, depth + 1);
            else if (S_ISREG(st.st_mode) && is_media_file(entry->d_name)) result = append_track(library, path);
        }
        free(path);
        if (result != ESP_OK) break;
    }
    closedir(dir);
    return result;
}

static int compare_tracks(const void *left, const void *right) {
    const media_track_t *a = left;
    const media_track_t *b = right;
    int result = strcasecmp(a->artist, b->artist);
    if (result == 0) result = strcasecmp(a->album, b->album);
    if (result == 0 && a->disc_number != b->disc_number) result = a->disc_number < b->disc_number ? -1 : 1;
    if (result == 0 && a->track_number != b->track_number) result = a->track_number < b->track_number ? -1 : 1;
    if (result == 0) result = strcasecmp(a->title, b->title);
    return result;
}

static esp_err_t build_indexes(media_library_t *library) {
    if (library->count == 0) return ESP_OK;
    library->artists = calloc(library->count, sizeof(media_artist_t));
    library->albums = calloc(library->count, sizeof(media_album_t));
    if (!library->artists || !library->albums) return ESP_ERR_NO_MEM;

    size_t artist = SIZE_MAX;
    size_t album = SIZE_MAX;
    for (size_t track = 0; track < library->count; track++) {
        media_track_t *item = &library->tracks[track];
        if (artist == SIZE_MAX || strcasecmp(library->artists[artist].name, item->artist) != 0) {
            artist = library->artist_count++;
            library->artists[artist] = (media_artist_t){
                .name = item->artist,
                .first_album = library->album_count,
            };
            album = SIZE_MAX;
        }
        if (album == SIZE_MAX || strcasecmp(library->albums[album].name, item->album) != 0) {
            album = library->album_count++;
            library->albums[album] = (media_album_t){
                .name = item->album,
                .artist_index = artist,
                .first_track = track,
            };
            library->artists[artist].album_count++;
        }
        library->artists[artist].track_count++;
        library->albums[album].track_count++;
    }
    return ESP_OK;
}

esp_err_t media_library_mount(void) {
    if (mounted_card != NULL) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount, &mounted_card);
    if (err != ESP_OK) {
        mounted_card = NULL;
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return ESP_OK;
}

void media_library_unmount(void) {
    if (mounted_card == NULL) return;
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);
    mounted_card = NULL;
}

esp_err_t media_library_scan(media_library_t *library) {
    if (library == NULL || mounted_card == NULL) return ESP_ERR_INVALID_STATE;
    media_library_clear(library);
    esp_err_t err = scan_directory(library, MOUNT_POINT, 0);
    if (err == ESP_OK && library->count > 1) qsort(library->tracks, library->count, sizeof(media_track_t), compare_tracks);
    if (err == ESP_OK) err = build_indexes(library);
    ESP_LOGI(TAG, "Library: %u tracks, %u artists, %u albums", (unsigned)library->count,
             (unsigned)library->artist_count, (unsigned)library->album_count);
    return err;
}

void media_library_clear(media_library_t *library) {
    if (library == NULL) return;
    for (size_t i = 0; i < library->count; i++) {
        free(library->tracks[i].path);
        free(library->tracks[i].title);
        free(library->tracks[i].artist);
        free(library->tracks[i].album);
        free(library->tracks[i].cover_path);
    }
    free(library->tracks);
    free(library->artists);
    free(library->albums);
    memset(library, 0, sizeof(*library));
}

size_t media_library_find_path(const media_library_t *library, const char *path) {
    if (!library || !path) return SIZE_MAX;
    for (size_t i = 0; i < library->count; i++) {
        if (strcmp(library->tracks[i].path, path) == 0) return i;
    }
    return SIZE_MAX;
}

const char *media_library_display_name(const char *path) {
    if (path == NULL) return "";
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}
