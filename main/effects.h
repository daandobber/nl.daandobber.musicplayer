#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "audio_analysis.h"
#include "pax_gfx.h"

typedef enum {
    EFFECT_PLASMA = 0,
    EFFECT_METABALLS,
    EFFECT_SPECTRUM,
    EFFECT_SCOPE,
    EFFECT_TUNNEL,
    EFFECT_RADIAL,
    EFFECT_STARFIELD,
    EFFECT_KALEIDO,
    EFFECT_FLUID,
    EFFECT_RIBBONS,
    EFFECT_MILKDROP_FIRST,
    EFFECT_PROCEDURAL_FIRST = EFFECT_MILKDROP_FIRST + 64,
    EFFECT_COUNT = EFFECT_PROCEDURAL_FIRST + 54,
} effect_id_t;

void        effects_init(size_t width, size_t height);
void        effects_next(void);
void        effects_previous(void);
void        effects_select(effect_id_t effect);
effect_id_t effects_current(void);
const char *effects_name(void);
const char *effects_name_for(effect_id_t effect);
pax_col_t   effects_palette_color(float position, float accent);
void        effects_set_overlay_visible(bool visible);
void        effects_set_intensity(uint8_t intensity);
void        effects_set_palette_controls(uint8_t mode, uint8_t palette, uint8_t speed);
void        effects_render(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt);
