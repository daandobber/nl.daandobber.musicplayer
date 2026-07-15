#pragma once

#include <stdbool.h>
#include "media_library.h"
#include "pax_gfx.h"

bool album_art_draw(pax_buf_t *buffer, const media_track_t *track, int x, int y, int size);
void album_art_invalidate(void);
