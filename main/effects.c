/*
 * Audio-reactive ports inspired by Mika Tuupola's esp_effects project.
 * Original effect algorithms are MIT-0 licensed.
 */
#include "effects.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

#define EFFECT_PIXEL 8
#define BALL_COUNT   3
#define SIN_LUT_SIZE 512
#define TWO_PI       6.28318530718f
#define EFFECT_TOP   0
#define EFFECT_BOTTOM 0
#define CARD_X       390
#define CARD_Y       344
#define CARD_W       394
#define CARD_H       120
#define STAR_COUNT   96

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
} effect_ball_t;

static size_t        screen_width;
static size_t        screen_height;
static effect_id_t   current_effect = EFFECT_PROCEDURAL_FIRST + 48;
static float         phase;
static uint8_t       hue;
static uint32_t      last_beat;
static effect_ball_t balls[BALL_COUNT];
static float         sin_lut[SIN_LUT_SIZE];
static float        *radial_lut;
static float        *angle_lut;
static float        *reaction_a;
static float        *reaction_b;
static float        *reaction_next_a;
static float        *reaction_next_b;
static uint8_t      *mind_cells;
static uint8_t      *mind_next;
static uint32_t      mind_lfsr = 0x6d2b79f5u;
static uint32_t      mind_last_beat;
static float         mind_step_time;
static size_t        grid_width;
static size_t        grid_height;
static bool          overlay_visible = true;
static uint8_t       visual_intensity = 1;
static pax_buf_t    *previous_buffer;
static effect_id_t   previous_effect = EFFECT_COUNT;
static uint16_t     *morph_pixels;
static int64_t       morph_start;
static uint8_t       morph_sequence;
static float         palette_cursor;
static uint32_t      palette_last_beat;

typedef struct { float x, y, z; } effect_star_t;
static effect_star_t stars[STAR_COUNT];

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static const uint32_t palette_colors[][3] = {
    {0x05000c, 0x7115a8, 0xffc8ff}, {0x090000, 0xc3260b, 0xffd05a},
    {0x000817, 0x075ca8, 0x72f1ff}, {0x090700, 0x8f6500, 0xfff1a6},
    {0x001316, 0x148d89, 0xe5fff4}, {0x110015, 0xb00074, 0xff93d7},
    {0x03030a, 0x3a36b8, 0x63ff9b}, {0x001006, 0x168843, 0xffd75a},
    {0x000000, 0x570000, 0xff3030}, {0x100100, 0xe13b00, 0xffe063},
    {0x000513, 0x083d91, 0xb8e8ff}, {0x030506, 0x69767d, 0xffffff},
    {0x09040d, 0x44265c, 0xd7a6e8}, {0x071000, 0x76a600, 0xeaff8b},
    {0x100b00, 0x9b4d12, 0xf4d6a0}, {0x000d10, 0x007e92, 0x4dffe1},
    {0x0d0010, 0x5c168f, 0xff6e47}, {0x020c05, 0x2b7244, 0xa6ffcf},
};

static pax_col_t palette_sample(size_t palette, float position, float accent) {
    size_t count = sizeof(palette_colors) / sizeof(palette_colors[0]);
    const uint32_t *stops = palette_colors[palette % count];
    float p = clamp01(position);
    int segment = p >= .5f;
    float mix = segment ? (p - .5f) * 2.0f : p * 2.0f;
    uint32_t a = stops[segment], b = stops[segment + 1];
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int red = ar + (int)((((b >> 16) & 255) - ar) * mix);
    int green = ag + (int)((((b >> 8) & 255) - ag) * mix);
    int blue = ab + (int)(((b & 255) - ab) * mix);
    float flash = accent * .16f;
    red += (int)((255 - red) * flash);
    green += (int)((255 - green) * flash);
    blue += (int)((255 - blue) * flash);
    return pax_col_rgb(red, green, blue);
}

static pax_col_t global_palette_color(float position, float accent) {
    size_t count = sizeof(palette_colors) / sizeof(palette_colors[0]);
    float wrapped = fmodf(palette_cursor, (float)count);
    if (wrapped < 0) wrapped += count;
    size_t first = (size_t)wrapped;
    size_t second = (first + 1) % count;
    float mix = wrapped - first;
    mix = mix * mix * (3.0f - 2.0f * mix);
    pax_col_t a = palette_sample(first, position, accent);
    pax_col_t b = palette_sample(second, position, accent);
    int red = (int)(((a >> 16) & 255) * (1.0f - mix) + ((b >> 16) & 255) * mix);
    int green = (int)(((a >> 8) & 255) * (1.0f - mix) + ((b >> 8) & 255) * mix);
    int blue = (int)((a & 255) * (1.0f - mix) + (b & 255) * mix);
    return pax_col_rgb(red, green, blue);
}

static void update_global_palette(const audio_analysis_snapshot_t *audio, float dt) {
    palette_cursor += dt * (.040f + audio->treble * .028f);
    if (audio->beat_counter != palette_last_beat) {
        palette_cursor += audio->beat_strength * .018f;
        palette_last_beat = audio->beat_counter;
    }
}

static void effect_rect(pax_buf_t *buffer, pax_col_t color, float x, float y, float width, float height) {
    if (!overlay_visible) {
        pax_simple_rect(buffer, color, x, y, width, height);
        return;
    }
    float right = x + width;
    float bottom = y + height;
    const float card_right = CARD_X + CARD_W;
    const float card_bottom = CARD_Y + CARD_H;
    if (right <= CARD_X || x >= card_right || bottom <= CARD_Y || y >= card_bottom) {
        pax_simple_rect(buffer, color, x, y, width, height);
        return;
    }
    if (y < CARD_Y) pax_simple_rect(buffer, color, x, y, width, CARD_Y - y);
    if (bottom > card_bottom) pax_simple_rect(buffer, color, x, card_bottom, width, bottom - card_bottom);
    float middle_top = fmaxf(y, CARD_Y);
    float middle_bottom = fminf(bottom, card_bottom);
    if (middle_bottom > middle_top) {
        if (x < CARD_X) pax_simple_rect(buffer, color, x, middle_top, CARD_X - x, middle_bottom - middle_top);
        if (right > card_right) {
            pax_simple_rect(buffer, color, card_right, middle_top, right - card_right, middle_bottom - middle_top);
        }
    }
}

void effects_init(size_t width, size_t height) {
    screen_width = width;
    screen_height = height;
    for (size_t i = 0; i < SIN_LUT_SIZE; i++) sin_lut[i] = sinf((float)i * TWO_PI / SIN_LUT_SIZE);

    grid_width = (width + EFFECT_PIXEL - 1) / EFFECT_PIXEL;
    grid_height = (height + EFFECT_PIXEL - 1) / EFFECT_PIXEL;
    free(radial_lut);
    radial_lut = malloc(grid_width * grid_height * sizeof(float));
    free(angle_lut);
    angle_lut = malloc(grid_width * grid_height * sizeof(float));
    if (radial_lut != NULL && angle_lut != NULL) {
        for (size_t gy = 0; gy < grid_height; gy++) {
            for (size_t gx = 0; gx < grid_width; gx++) {
                float x = gx * EFFECT_PIXEL + EFFECT_PIXEL * 0.5f - width * 0.5f;
                float y = gy * EFFECT_PIXEL + EFFECT_PIXEL * 0.5f - height * 0.5f;
                radial_lut[gy * grid_width + gx] = sqrtf(x * x + y * y);
                angle_lut[gy * grid_width + gx] = atan2f(y, x);
            }
        }
    }
    size_t cells = grid_width * grid_height;
    free(reaction_a); free(reaction_b); free(reaction_next_a); free(reaction_next_b);
    reaction_a = malloc(cells * sizeof(float));
    reaction_b = malloc(cells * sizeof(float));
    reaction_next_a = malloc(cells * sizeof(float));
    reaction_next_b = malloc(cells * sizeof(float));
    if (reaction_a && reaction_b && reaction_next_a && reaction_next_b) {
        for (size_t i = 0; i < cells; i++) {
            reaction_a[i] = reaction_next_a[i] = 1.0f;
            reaction_b[i] = reaction_next_b[i] = 0.0f;
        }
    }
    free(mind_cells); free(mind_next);
    mind_cells = calloc(cells, sizeof(uint8_t));
    mind_next = calloc(cells, sizeof(uint8_t));
    for (size_t i = 0; i < BALL_COUNT; i++) {
        balls[i].x = width * (0.2f + i * 0.3f);
        balls[i].y = height * (0.25f + (i & 1) * 0.45f);
        balls[i].vx = 55.0f + i * 17.0f;
        balls[i].vy = 43.0f + i * 13.0f;
        if (i & 1) balls[i].vx = -balls[i].vx;
    }
    uint32_t random = 0x5eed1234;
    for (size_t i = 0; i < STAR_COUNT; i++) {
        random = random * 1664525u + 1013904223u;
        stars[i].x = ((int)(random & 0xffff) - 32768) / 32768.0f;
        random = random * 1664525u + 1013904223u;
        stars[i].y = ((int)(random & 0xffff) - 32768) / 32768.0f;
        stars[i].z = 0.08f + ((random >> 16) & 0xff) / 280.0f;
    }
}

static inline float fast_sin(float radians) {
    int index = (int)(radians * ((float)SIN_LUT_SIZE / TWO_PI));
    index %= SIN_LUT_SIZE;
    if (index < 0) index += SIN_LUT_SIZE;
    return sin_lut[index];
}

static inline void fill_effect_cell(pax_buf_t *buffer, size_t x, size_t y, pax_col_t color) {
    size_t width = EFFECT_PIXEL;
    size_t height = EFFECT_PIXEL;
    if (x + width > screen_width) width = screen_width - x;
    if (y + height > screen_height) height = screen_height - y;
    if (overlay_visible && x < CARD_X + CARD_W && x + width > CARD_X && y < CARD_Y + CARD_H && y + height > CARD_Y) return;

    // Tanmatsu uses a 480x800 RGB565 framebuffer viewed with PAX_O_ROT_CW.
    // Filling the corresponding physical rows directly avoids thousands of
    // transformed PAX rectangle dispatches per frame.
    if (buffer->orientation == PAX_O_ROT_CW && buffer->buf_16bpp != NULL) {
        uint16_t pixel = (uint16_t)buffer->col2buf(buffer, color);
        size_t raw_width = (size_t)buffer->width;
        size_t first_column = raw_width - y - height;
        for (size_t logical_x = x; logical_x < x + width; logical_x++) {
            uint16_t *destination = buffer->buf_16bpp + logical_x * raw_width + first_column;
            for (size_t column = 0; column < height; column++) destination[column] = pixel;
        }
    } else {
        pax_simple_rect(buffer, color, x, y, width, height);
    }
}

static void begin_morph(effect_id_t destination) {
    if (!previous_buffer || !previous_buffer->buf_16bpp) return;
    size_t pixels = (size_t)previous_buffer->width * previous_buffer->height;
    uint16_t *snapshot = realloc(morph_pixels, pixels * sizeof(uint16_t));
    if (!snapshot) return;
    morph_pixels = snapshot;
    memcpy(morph_pixels, previous_buffer->buf_16bpp, pixels * sizeof(uint16_t));
    morph_sequence++;
    morph_start = esp_timer_get_time();
}

void effects_next(void) {
    begin_morph((current_effect + 1) % EFFECT_COUNT);
    current_effect = (current_effect + 1) % EFFECT_COUNT;
}

void effects_previous(void) {
    begin_morph((current_effect + EFFECT_COUNT - 1) % EFFECT_COUNT);
    current_effect = (current_effect + EFFECT_COUNT - 1) % EFFECT_COUNT;
}

void effects_select(effect_id_t effect) {
    if (effect < EFFECT_COUNT && effect != current_effect) {
        begin_morph(effect);
        current_effect = effect;
    }
}

effect_id_t effects_current(void) {
    return current_effect;
}

const char *effects_name(void) {
    static const char *legacy_names[] = {
        "Plasma", "Metaballs", "Spectrum", "Neon scope", "Warp tunnel", "Radial pulse", "Star rush", "Kaleido",
        "Liquid feedback", "Wave ribbons"
    };
    static const char *milk_names[] = {
        "Nebula bloom", "Hyperspace flower", "Solar vortex", "Acid glass",
        "Velvet cyclone", "Electric iris", "Molten cathedral", "Prism implosion",
        "Deep sea portal", "Chromatic smoke", "Fractal heartbeat", "Aurora engine",
        "Liquid mandala", "Neon singularity", "Cosmic cells", "Plasma cathedral",
        "Ruby shockwave", "Emerald dream", "Ultraviolet tide", "Golden reactor",
        "Bass gravity", "Treble storm", "Spectrum warp", "Beat supernova",
        "Mirror dimension", "Crystal tunnel", "Recursive sun", "Psychedelic ink",
        "Quantum petals", "Lava lens", "Cyber whirlpool", "Infinite bloom",
        "Polar thunder", "Mobius candy", "Checker implosion", "Digital waterfall",
        "Laser smear", "Binary suns", "Spiral orchid", "Diamond crusher",
        "Elastic cosmos", "Audio spokes", "Zigzag nebula", "Orbital gravity",
        "Logarithmic lava", "Interference dream", "Ring accelerator", "Beat cannon",
        "Rotating tiles", "Frozen vortex", "Toxic honey", "Candy shockwave",
        "Infrared ocean", "Emerald machine", "Monochrome ghost", "CMY furnace",
        "Bass amoeba", "Midrange maze", "Treble diamonds", "Full spectrum storm",
        "Velvet recursion", "Chrome petals", "Radioactive portal", "Final ascension"
    };
    static const char *procedural_names[] = {
        "Reaction bloom", "Voronoi lava", "Julia dream", "Mandelbrot pulse",
        "Moire silk", "Lissajous field", "Audio mosaic", "Polar mandala",
        "Checker abyss", "Cellular flame", "Bass ripples", "Spectral crystal",
        "Velvet clouds", "Superformula beast", "Marble domain", "Truchet circuit",
        "Hex reactor", "Magnetic flux", "Rose engine", "Ocean caustics",
        "Synth terrain", "Interference beads", "Plasma colonies", "Square recursion",
        "Infinite fractal zoom", "Vector vortex", "Strange geometry", "Recursive portals",
        "Minimal planes", "Bass monolith", "Quiet horizon", "Bauhaus rhythm",
        "Diagonal silence", "Orbit fields", "Negative space", "Audio stripes",
        "Mondrian pulse", "Four colour blocks", "Sparse tunnel", "Twin suns",
        "Fractal caustics", "Voronoi checker", "Magnetic mandala", "Marble circuit",
        "Crystal vortex", "Chemical geometry", "Mosaic fractal", "Infinite flora",
        "Mind Is Growing", "Copper tide", "Kefrens echoes", "Rotozoom tiles",
        "Shadebob trails", "Scene twister"
    };
    if (current_effect < EFFECT_MILKDROP_FIRST) return legacy_names[current_effect];
    if (current_effect < EFFECT_PROCEDURAL_FIRST) return milk_names[current_effect - EFFECT_MILKDROP_FIRST];
    return procedural_names[current_effect - EFFECT_PROCEDURAL_FIRST];
}

void effects_set_overlay_visible(bool visible) {
    overlay_visible = visible;
}

void effects_set_intensity(uint8_t intensity) {
    visual_intensity = intensity > 2 ? 2 : intensity;
}

static void render_plasma(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (2.2f + audio->rms * 6.0f + audio->bass * 3.0f);
    if (audio->beat_counter != last_beat) {
        hue += 23;
        last_beat = audio->beat_counter;
    }

    for (size_t y = EFFECT_TOP; y < screen_height - EFFECT_BOTTOM; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            float scale = 30.0f - audio->bass * 12.0f;
            float v1 = fast_sin(x / scale + phase);
            float v2 = fast_sin(y / (24.0f + audio->mid * 15.0f) - phase * 0.7f);
            size_t radial_index = (y / EFFECT_PIXEL) * grid_width + (x / EFFECT_PIXEL);
            float distance = radial_lut ? radial_lut[radial_index]
                                        : hypotf((float)x - screen_width * 0.5f,
                                                 (float)y - screen_height * 0.5f);
            float v3 = fast_sin(distance / (26.0f - audio->bass * 8.0f) + phase);
            float mixed = (v1 + v2 + v3 + 3.0f) / 6.0f;
            uint8_t value = 35 + (uint8_t)(clamp01(mixed + audio->beat_strength * 0.35f) * 220.0f);
            fill_effect_cell(buffer, x, y, global_palette_color(value / 255.0f, audio->beat_strength));
        }
    }
}

static void animate_balls(const audio_analysis_snapshot_t *audio, float dt) {
    float speed = 1.25f + audio->mid * 2.4f;
    for (size_t i = 0; i < BALL_COUNT; i++) {
        balls[i].x += balls[i].vx * dt * speed;
        balls[i].y += balls[i].vy * dt * speed;
        if (balls[i].x < 0) {
            balls[i].x = 0;
            balls[i].vx = fabsf(balls[i].vx);
        } else if (balls[i].x > screen_width) {
            balls[i].x = screen_width;
            balls[i].vx = -fabsf(balls[i].vx);
        }
        if (balls[i].y < 0) {
            balls[i].y = 0;
            balls[i].vy = fabsf(balls[i].vy);
        } else if (balls[i].y > screen_height) {
            balls[i].y = screen_height;
            balls[i].vy = -fabsf(balls[i].vy);
        }
    }
}

static void render_metaballs(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    animate_balls(audio, dt);
    if (audio->beat_counter != last_beat) {
        hue += 31;
        last_beat = audio->beat_counter;
    }
    effect_rect(buffer, 0xff02030a, 0, EFFECT_TOP, screen_width,
                screen_height - EFFECT_TOP - EFFECT_BOTTOM);

    float radius = 58.0f + audio->bass * 75.0f + audio->beat_strength * 30.0f;
    for (size_t y = EFFECT_TOP; y < screen_height - EFFECT_BOTTOM; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            float field = 0.0f;
            for (size_t i = 0; i < BALL_COUNT; i++) {
                float dx = x - balls[i].x;
                float dy = y - balls[i].y;
                field += radius * radius / fmaxf(dx * dx + dy * dy, 16.0f);
            }
            if (field > 0.32f) {
                float strength = clamp01((field - 0.32f) * 1.8f);
                uint8_t value = 70 + strength * 185;
                fill_effect_cell(buffer, x, y,
                                 global_palette_color(value / 255.0f, audio->beat_strength));
            }
        }
    }
}

static void render_spectrum(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    (void)dt;
    effect_rect(buffer, 0xff03030a, 0, EFFECT_TOP, screen_width,
                screen_height - EFFECT_TOP - EFFECT_BOTTOM);
    float margin = 30.0f;
    float gap = 5.0f;
    float bar_width = (screen_width - margin * 2 - gap * (AUDIO_ANALYSIS_BANDS - 1)) / AUDIO_ANALYSIS_BANDS;
    float center = EFFECT_TOP + (screen_height - EFFECT_TOP - EFFECT_BOTTOM) * 0.5f;
    float usable_height = (screen_height - EFFECT_TOP - EFFECT_BOTTOM - 18.0f) * 0.5f;
    for (size_t i = 0; i < AUDIO_ANALYSIS_BANDS; i++) {
        float value = clamp01(audio->bands[i]);
        float height = fmaxf(3.0f, value * usable_height);
        float x = margin + i * (bar_width + gap);
        pax_col_t color = global_palette_color(.35f + value * .65f, audio->beat_strength);
        effect_rect(buffer, 0xff101426, x, center - usable_height, bar_width, usable_height * 2);
        effect_rect(buffer, color, x, center - height, bar_width, height * 2);
    }
    if (audio->beat_counter != last_beat) {
        hue += 19;
        last_beat = audio->beat_counter;
    }
}

static inline void set_effect_pixel(pax_buf_t *buffer, int x, int y, uint16_t pixel) {
    if (x < 0 || y < EFFECT_TOP || x >= (int)screen_width || y >= (int)(screen_height - EFFECT_BOTTOM)) return;
    if (overlay_visible && x >= CARD_X && x < CARD_X + CARD_W && y >= CARD_Y && y < CARD_Y + CARD_H) return;
    if (buffer->orientation == PAX_O_ROT_CW && buffer->buf_16bpp != NULL) {
        buffer->buf_16bpp[x * buffer->width + (buffer->width - 1 - y)] = pixel;
    }
}

static void draw_effect_line(pax_buf_t *buffer, int x0, int y0, int x1, int y1, pax_col_t color, int thickness) {
    uint16_t pixel = (uint16_t)buffer->col2buf(buffer, color);
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        for (int offset = -thickness / 2; offset <= thickness / 2; offset++) {
            set_effect_pixel(buffer, x0, y0 + offset, pixel);
        }
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void render_scope(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    (void)dt;
    effect_rect(buffer, 0xff02050d, 0, EFFECT_TOP, screen_width,
                screen_height - EFFECT_TOP - EFFECT_BOTTOM);

    int content_height = screen_height - EFFECT_TOP - EFFECT_BOTTOM;
    int center = EFFECT_TOP + content_height / 2;
    for (int row = 1; row < 4; row++) {
        int y = EFFECT_TOP + row * content_height / 4;
        draw_effect_line(buffer, 0, y, screen_width - 1, y, 0xff10182a, 1);
    }

    float band_width = (float)screen_width / AUDIO_ANALYSIS_BANDS;
    for (size_t band = 0; band < AUDIO_ANALYSIS_BANDS; band++) {
        float strength = clamp01(audio->bands[band]);
        float height = 8.0f + strength * content_height * 0.32f;
        float x = band * band_width + 3.0f;
        effect_rect(buffer, global_palette_color(.12f + strength * .38f, audio->beat_strength), x,
                    center - height, band_width - 6.0f, height * 2.0f);
    }

    float amplitude = content_height * (0.22f + audio->rms * 0.34f);
    int previous_x = 0;
    int previous_y = center;
    int previous_mirror = center;
    for (size_t i = 0; i < AUDIO_ANALYSIS_WAVEFORM; i++) {
        int x = i * (screen_width - 1) / (AUDIO_ANALYSIS_WAVEFORM - 1);
        float sample = audio->waveform[i] / 32768.0f;
        int y = center + (int)(sample * amplitude);
        int mirror = center - (int)(sample * amplitude * 0.72f);
        if (i > 0) {
            draw_effect_line(buffer, previous_x, previous_mirror, x, mirror,
                             global_palette_color(.45f + audio->treble * .35f, audio->beat_strength), 3);
            draw_effect_line(buffer, previous_x, previous_y, x, y,
                             global_palette_color(.82f, audio->beat_strength), 3);
        }
        previous_x = x;
        previous_y = y;
        previous_mirror = mirror;
    }
    if (audio->beat_counter != last_beat) {
        hue += 17;
        last_beat = audio->beat_counter;
    }
}

static void render_tunnel(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (3.5f + audio->bass * 8.0f);
    if (audio->beat_counter != last_beat) { hue += 29; last_beat = audio->beat_counter; }
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            size_t index = (y / EFFECT_PIXEL) * grid_width + x / EFFECT_PIXEL;
            float radius = radial_lut ? radial_lut[index] : 0.0f;
            float angle = angle_lut ? angle_lut[index] : 0.0f;
            float rings = fast_sin(radius * (0.055f + audio->mid * 0.025f) - phase * 2.8f);
            float ribs = fast_sin(angle * (6.0f + audio->treble * 5.0f) + phase);
            float value = clamp01((rings + ribs + 1.4f) * 0.36f + audio->beat_strength * 0.28f);
            fill_effect_cell(buffer, x, y, global_palette_color(.08f + value * .92f, audio->beat_strength));
        }
    }
}

static void render_radial(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (1.2f + audio->mid * 3.0f);
    effect_rect(buffer, 0xff02030b, 0, 0, screen_width, screen_height);
    int cx = screen_width / 2;
    int cy = screen_height / 2;
    float base = 62.0f + audio->rms * 42.0f;
    for (size_t band = 0; band < AUDIO_ANALYSIS_BANDS; band++) {
        float angle = phase * 0.35f + band * TWO_PI / AUDIO_ANALYSIS_BANDS;
        float strength = clamp01(audio->bands[band]);
        float inner = base + fast_sin(phase * 2.0f + band) * 12.0f;
        float outer = inner + 55.0f + strength * 190.0f;
        int x0 = cx + fast_sin(angle + TWO_PI * 0.25f) * inner;
        int y0 = cy + fast_sin(angle) * inner;
        int x1 = cx + fast_sin(angle + TWO_PI * 0.25f) * outer;
        int y1 = cy + fast_sin(angle) * outer;
        draw_effect_line(buffer, x0, y0, x1, y1,
                         global_palette_color(.25f + strength * .75f, audio->beat_strength), 5);
    }
    if (audio->beat_counter != last_beat) { hue += 37; last_beat = audio->beat_counter; }
}

static void render_starfield(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    effect_rect(buffer, 0xff01030a, 0, 0, screen_width, screen_height);
    float speed = dt * (0.18f + audio->rms * 0.75f + audio->beat_strength * 0.55f);
    int cx = screen_width / 2;
    int cy = screen_height / 2;
    for (size_t i = 0; i < STAR_COUNT; i++) {
        stars[i].z -= speed;
        if (stars[i].z < 0.025f) {
            stars[i].z = 0.9f;
            stars[i].x = fast_sin(i * 1.731f + phase * 0.7f);
            stars[i].y = fast_sin(i * 2.417f + phase * 1.1f);
        }
        int x = cx + stars[i].x * 185.0f / stars[i].z;
        int y = cy + stars[i].y * 115.0f / stars[i].z;
        int size = stars[i].z < 0.18f ? 3 : (stars[i].z < 0.42f ? 2 : 1);
        uint16_t pixel = (uint16_t)buffer->col2buf(buffer,
            global_palette_color(.35f + (1.0f - stars[i].z) * .65f, audio->beat_strength));
        for (int oy = -size; oy <= size; oy++) for (int ox = -size; ox <= size; ox++)
            set_effect_pixel(buffer, x + ox, y + oy, pixel);
    }
    phase += dt * 2.0f;
    if (audio->beat_counter != last_beat) { hue += 21; last_beat = audio->beat_counter; }
}

static void render_kaleido(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (0.9f + audio->treble * 2.8f);
    if (audio->beat_counter != last_beat) { hue += 43; last_beat = audio->beat_counter; }
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            size_t index = (y / EFFECT_PIXEL) * grid_width + x / EFFECT_PIXEL;
            float radius = radial_lut ? radial_lut[index] : 0.0f;
            float angle = angle_lut ? fabsf(angle_lut[index]) : 0.0f;
            float folded = fabsf(fmodf(angle * 4.0f + phase, TWO_PI) - 3.14159265f);
            float pattern = fast_sin(radius * 0.045f + folded * 3.0f - phase * 2.0f);
            float pulse = fast_sin(radius * 0.018f - phase) * audio->bass;
            float value = clamp01(0.42f + pattern * 0.38f + pulse * 0.3f + audio->beat_strength * 0.3f);
            fill_effect_cell(buffer, x, y, global_palette_color(.06f + value * .94f, audio->beat_strength));
        }
    }
}

static void render_fluid(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (0.55f + audio->mid * 1.4f);
    bool seed = previous_buffer == NULL || previous_buffer == buffer || previous_effect != EFFECT_FLUID;
    float cx = screen_width * 0.5f;
    float cy = screen_height * 0.5f;
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            pax_col_t color;
            if (seed) {
                float wave = (fast_sin(x * 0.018f + phase) + fast_sin(y * 0.025f - phase) + 2.0f) * 0.25f;
                color = global_palette_color(.08f + wave * .55f, audio->beat_strength);
            } else {
                float dx = x - cx;
                float dy = y - cy;
                float radius = sqrtf(dx * dx + dy * dy) + 1.0f;
                float warp = 5.0f + audio->bass * 16.0f;
                int sx = (int)(x + (-dy / radius) * warp + fast_sin(y * 0.018f + phase) * 3.0f);
                int sy = (int)(y + ( dx / radius) * warp + fast_sin(x * 0.015f - phase) * 3.0f);
                if (sx < 0) sx = 0; else if (sx >= (int)screen_width) sx = screen_width - 1;
                if (sy < 0) sy = 0; else if (sy >= (int)screen_height) sy = screen_height - 1;
                if (overlay_visible && sx >= CARD_X && sx < CARD_X + CARD_W && sy >= CARD_Y && sy < CARD_Y + CARD_H) {
                    color = 0xff03050b;
                } else {
                    uint16_t raw = previous_buffer->buf_16bpp[sx * previous_buffer->width +
                                                              (previous_buffer->width - 1 - sy)];
                    pax_col_t old = previous_buffer->buf2col(previous_buffer, raw);
                    int red = (old >> 16) & 0xff;
                    int green = (old >> 8) & 0xff;
                    int blue = old & 0xff;
                    float decay = 0.955f + audio->rms * 0.025f;
                    red *= decay; green *= decay; blue *= decay;
                    uint8_t injection = (uint8_t)(audio->beat_strength * 22.0f + audio->treble * 5.0f);
                    pax_col_t injected = global_palette_color(.72f, audio->beat_strength);
                    red += injection * ((injected >> 16) & 255) / 255;
                    green += injection * ((injected >> 8) & 255) / 255;
                    blue += injection * (injected & 255) / 255;
                    if (red > 255) red = 255;
                    if (green > 255) green = 255;
                    if (blue > 255) blue = 255;
                    color = pax_col_rgb(red, green, blue);
                }
            }
            fill_effect_cell(buffer, x, y, color);
        }
    }
    if (audio->beat_counter != last_beat) { hue += 7; last_beat = audio->beat_counter; }
}

static void render_ribbons(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    phase += dt * (0.7f + audio->mid * 1.8f);
    effect_rect(buffer, 0xff02040b, 0, 0, screen_width, screen_height);
    for (int ribbon = 0; ribbon < 7; ribbon++) {
        int previous_x = 0;
        int previous_y = screen_height / 2;
        float offset = (ribbon - 3) * 29.0f;
        float amplitude = 30.0f + audio->rms * 105.0f + ribbon * 3.0f;
        for (int x = 0; x < (int)screen_width; x += 5) {
            size_t sample_index = (size_t)x * (AUDIO_ANALYSIS_WAVEFORM - 1) / (screen_width - 1);
            float sample = audio->waveform[sample_index] / 32768.0f;
            float wave = fast_sin(x * 0.018f + phase * (1.0f + ribbon * 0.08f) + ribbon * 0.8f);
            int y = screen_height / 2 + offset + (int)(wave * amplitude * 0.55f + sample * amplitude);
            if (x > 0) draw_effect_line(buffer, previous_x, previous_y, x, y,
                                        global_palette_color(.32f +
                                            audio->bands[ribbon % AUDIO_ANALYSIS_BANDS] * .68f,
                                            audio->beat_strength), 3);
            previous_x = x;
            previous_y = y;
        }
    }
    if (audio->beat_counter != last_beat) { hue += 5; last_beat = audio->beat_counter; }
}

typedef struct {
    float zoom;
    float rotation;
    float ripple;
    float radial_frequency;
    float angular_frequency;
    float decay;
    float speed;
    float injection;
    uint8_t hue_offset;
    uint8_t warp;
    uint8_t palette;
} milk_field_preset_t;

// A compact, equation-driven preset bank. Each entry changes the feedback
// transform, oscillator field, colour injection and audio coupling. This is
// deliberately data-driven, like MilkDrop presets, but uses the P4 CPU instead
// of OpenGL shaders.
static const milk_field_preset_t milk_presets[64] = {
    {-0.010f,  0.010f,  7, .024f,  3, .965f, .75f,  9,  12, 0, 0},
    { 0.014f, -0.018f, 12, .041f,  8, .958f, 1.05f, 11,  78, 5, 1},
    {-0.018f,  0.030f,  5, .018f,  5, .972f, .55f,  8, 145, 2, 2},
    { 0.008f, -0.007f, 17, .055f, 11, .952f, 1.30f, 13, 205, 1, 3},
    {-0.006f,  0.022f, 14, .031f,  6, .961f, .82f, 10, 226, 5, 4},
    { 0.020f, -0.011f, 10, .065f, 12, .948f, 1.42f, 14, 168, 4, 1},
    {-0.023f,  0.006f, 19, .016f,  4, .974f, .44f,  7,  20, 3, 5},
    { 0.011f,  0.028f,  8, .048f,  9, .956f, 1.18f, 12, 110, 4, 2},
    {-0.014f, -0.014f, 23, .022f,  7, .968f, .63f,  9, 152, 2, 0},
    { 0.004f,  0.009f, 28, .037f,  3, .950f, 1.55f, 15, 192, 1, 3},
    {-0.019f,  0.017f, 11, .058f, 10, .970f, .91f, 10, 238, 6, 4},
    { 0.017f, -0.024f, 16, .027f,  5, .955f, 1.10f, 12,  91, 0, 1},
    {-0.008f,  0.013f, 20, .044f, 14, .963f, .70f, 11, 132, 4, 2},
    { 0.026f,  0.035f,  6, .020f,  2, .946f, 1.62f, 16, 181, 5, 3},
    {-0.016f, -0.005f, 25, .052f,  8, .971f, .50f,  8,  55, 6, 5},
    { 0.007f,  0.019f, 13, .033f, 16, .959f, 1.25f, 13, 218, 3, 0},
    {-0.025f, -0.029f,  9, .069f,  6, .966f, .86f, 12,   2, 2, 1},
    { 0.013f,  0.008f, 22, .025f,  9, .953f, 1.38f, 14,  93, 1, 4},
    {-0.011f,  0.026f, 15, .046f, 13, .969f, .67f,  9, 190, 0, 3},
    { 0.022f, -0.016f, 18, .029f,  4, .951f, 1.48f, 15,  34, 5, 2},
    {-0.021f,  0.012f, 30, .019f,  7, .973f, .58f,  9, 160, 2, 5},
    { 0.009f, -0.032f, 12, .061f, 15, .947f, 1.72f, 16, 214, 6, 1},
    {-0.013f,  0.004f, 24, .036f, 11, .964f, .96f, 13, 104, 3, 4},
    { 0.019f,  0.023f, 17, .050f,  5, .949f, 1.57f, 18, 246, 0, 0},
    {-0.007f, -0.020f, 21, .028f, 12, .970f, .73f, 10, 126, 4, 5},
    { 0.016f,  0.015f,  9, .073f, 18, .954f, 1.33f, 14, 177, 6, 2},
    {-0.028f,  0.038f,  7, .014f,  3, .975f, .39f,  7,  43, 5, 3},
    { 0.005f, -0.009f, 33, .042f,  8, .945f, 1.81f, 17, 230, 1, 4},
    {-0.017f,  0.021f, 14, .057f, 20, .967f, .88f, 11, 142, 4, 1},
    { 0.024f, -0.027f, 26, .023f,  6, .952f, 1.46f, 15,  15, 2, 0},
    {-0.009f,  0.033f, 19, .039f, 10, .962f, 1.14f, 13, 198, 5, 5},
    { 0.012f, -0.012f, 29, .047f, 17, .957f, 1.66f, 16,  73, 3, 2},
    {-0.015f,  0.018f, 18, .032f,  9, .966f, 1.12f, 13, 208,  7,  6},
    { 0.009f, -0.027f, 22, .045f,  6, .953f, 1.48f, 16,  18,  8,  7},
    {-0.022f,  0.007f, 16, .026f, 12, .971f,  .76f, 11, 112,  9,  8},
    { 0.006f,  0.014f, 27, .058f,  4, .948f, 1.72f, 18, 176, 10,  9},
    {-0.011f, -0.019f, 31, .021f,  8, .963f, 1.30f, 15, 232, 11, 10},
    { 0.018f,  0.031f, 12, .067f, 14, .956f, 1.04f, 14,  64, 12, 11},
    {-0.026f, -0.008f, 20, .038f,  7, .973f,  .61f,  9, 146, 13,  6},
    { 0.014f,  0.023f, 24, .049f, 16, .950f, 1.56f, 17,  94, 14,  7},
    {-0.008f, -0.034f, 15, .029f,  5, .968f,  .88f, 12, 194, 15,  8},
    { 0.023f,  0.011f, 28, .054f, 11, .947f, 1.84f, 19,  36, 16,  9},
    {-0.019f,  0.026f, 19, .017f,  3, .972f,  .69f, 10, 220, 17, 10},
    { 0.010f, -0.016f, 33, .043f, 13, .952f, 1.41f, 16, 124, 18, 11},
    {-0.024f,  0.036f, 11, .063f, 18, .969f,  .93f, 12, 164, 19,  6},
    { 0.007f, -0.006f, 26, .035f, 10, .949f, 1.63f, 18,  78, 20,  7},
    {-0.013f,  0.020f, 17, .052f, 15, .965f, 1.20f, 14, 244, 21,  8},
    { 0.027f, -0.029f, 23, .024f,  6, .944f, 1.92f, 21,  10, 22,  9},
    {-0.017f,  0.009f, 14, .071f, 20, .970f,  .57f,  9, 188, 23, 10},
    { 0.012f,  0.028f, 30, .031f,  8, .951f, 1.52f, 17,  52,  7, 11},
    {-0.021f, -0.013f, 21, .047f, 12, .967f,  .81f, 11, 134,  8,  6},
    { 0.016f,  0.017f, 25, .056f, 17, .946f, 1.77f, 20, 202,  9,  7},
    {-0.007f, -0.025f, 18, .019f,  4, .974f,  .48f,  8,  26, 10,  8},
    { 0.020f,  0.005f, 29, .064f, 14, .949f, 1.68f, 19, 158, 11,  9},
    {-0.028f,  0.032f, 13, .040f,  9, .971f,  .72f, 10,  86, 12, 10},
    { 0.008f, -0.021f, 34, .027f,  7, .945f, 1.88f, 21, 226, 13, 11},
    {-0.016f,  0.012f, 16, .060f, 19, .964f, 1.08f, 14, 116, 14,  6},
    { 0.025f,  0.024f, 22, .034f, 11, .948f, 1.59f, 18, 172, 15,  7},
    {-0.010f, -0.031f, 27, .051f, 15, .969f,  .84f, 12,  44, 16,  8},
    { 0.015f,  0.008f, 19, .023f,  5, .952f, 1.45f, 17, 214, 17,  9},
    {-0.023f,  0.029f, 31, .068f, 21, .966f, 1.16f, 15, 102, 18, 10},
    { 0.011f, -0.010f, 24, .037f, 13, .947f, 1.74f, 20, 248, 19, 11},
    {-0.018f,  0.035f, 20, .046f, 16, .970f,  .66f, 10,  70, 20,  6},
    { 0.021f, -0.023f, 28, .030f, 10, .943f, 1.98f, 22, 196, 21,  7},
};

static void milk_palette(uint8_t palette, float color_phase, int injection,
                         int *red, int *green, int *blue) {
    (void)palette;
    float position = fast_sin(color_phase) * .5f + .5f;
    pax_col_t color = global_palette_color(position, 0.0f);
    *red += injection * ((color >> 16) & 255) / 255;
    *green += injection * ((color >> 8) & 255) / 255;
    *blue += injection * (color & 255) / 255;
}

static void render_milk_field(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt,
                              effect_id_t effect) {
    const milk_field_preset_t *preset = &milk_presets[effect - EFFECT_MILKDROP_FIRST];
    phase += dt * preset->speed * (0.62f + audio->mid * 0.85f);
    float cx = screen_width * 0.5f;
    float cy = screen_height * 0.5f;
    bool seed = !previous_buffer || previous_buffer == buffer || previous_effect != effect || !angle_lut || !radial_lut;
    float rotation = preset->rotation * (1.0f + audio->treble * 2.0f);
    float sin_rotation = fast_sin(rotation);
    float cos_rotation = fast_sin(rotation + TWO_PI * 0.25f);
    float zoom = 1.0f + preset->zoom * (1.0f + audio->bass * 2.2f);
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            size_t index = (y / EFFECT_PIXEL) * grid_width + x / EFFECT_PIXEL;
            float radius = radial_lut ? radial_lut[index] : 0.0f;
            float angle = angle_lut ? angle_lut[index] : 0.0f;
            float field = fast_sin(radius * preset->radial_frequency - phase * 2.0f +
                                   angle * preset->angular_frequency);
            pax_col_t color;
            if (seed) {
                float light = clamp01(0.24f + field * 0.22f + audio->rms * 0.5f);
                color = global_palette_color(light, audio->beat_strength);
            } else {
                float dx = x - cx;
                float dy = y - cy;
                float ripple = field * preset->ripple * (0.30f + audio->mid * 1.15f);
                float rx = (dx * cos_rotation - dy * sin_rotation) * zoom;
                float ry = (dx * sin_rotation + dy * cos_rotation) * zoom;
                float sx_float = cx + rx;
                float sy_float = cy + ry;
                switch (preset->warp) {
                    case 0: { // radial breathing / flower
                        float scale = 1.0f + ripple / (radius + 38.0f);
                        sx_float = cx + rx * scale; sy_float = cy + ry * scale;
                        break;
                    }
                    case 1: // crossed liquid waves
                        sx_float += fast_sin(y * .021f + phase * 1.7f) * preset->ripple;
                        sy_float += fast_sin(x * .017f - phase * 1.3f) * preset->ripple;
                        break;
                    case 2: { // bass lens
                        float lens = ripple / (radius * .45f + 24.0f) + audio->bass * .025f;
                        sx_float = cx + rx * (1.0f + lens); sy_float = cy + ry * (1.0f + lens);
                        break;
                    }
                    case 3: // oscillating shear
                        sx_float += ry * fast_sin(phase + radius * .011f) * .035f + ripple;
                        sy_float += rx * fast_sin(phase * .73f - radius * .014f) * .028f;
                        break;
                    case 4: { // kaleidoscope fold
                        float folded = fabsf(fmodf(angle * preset->angular_frequency + phase, TWO_PI) - 3.1415927f);
                        float source_angle = folded / fmaxf(preset->angular_frequency, 1.0f) + phase * .08f;
                        sx_float = cx + fast_sin(source_angle + TWO_PI * .25f) * radius * zoom;
                        sy_float = cy + fast_sin(source_angle) * radius * zoom;
                        break;
                    }
                    case 5: { // true vortex twist
                        float twist = preset->ripple / (radius + 30.0f) + field * .025f;
                        float sa = fast_sin(angle + twist + phase * .025f);
                        float ca = fast_sin(angle + twist + phase * .025f + TWO_PI * .25f);
                        sx_float = cx + ca * radius * zoom; sy_float = cy + sa * radius * zoom;
                        break;
                    }
                    case 6: { // spectrum-driven cellular grid
                        int band = ((int)(fabsf(angle) * 5.1f)) % AUDIO_ANALYSIS_BANDS;
                        float shove = preset->ripple * (audio->bands[band] + .18f);
                        sx_float += fast_sin(y * .034f + phase + band) * shove;
                        sy_float += fast_sin(x * .029f - phase + band) * shove;
                        break;
                    }
                    case 7: { // polar thunder: radial and angular waves collide
                        float bolt = fast_sin(angle * preset->angular_frequency + phase * 2.4f);
                        float ring = fast_sin(radius * preset->radial_frequency - phase * 3.1f);
                        sx_float += (bolt * dy / (radius + 12.0f) + ring) * preset->ripple;
                        sy_float += (-bolt * dx / (radius + 12.0f) + ring) * preset->ripple;
                        break;
                    }
                    case 8: { // Mobius-style alternating squeeze
                        float side = fast_sin(angle + phase * .7f);
                        float squeeze = 1.0f + side * .11f * (1.0f + audio->mid);
                        sx_float = cx + rx * squeeze + ripple;
                        sy_float = cy + ry / squeeze - ripple * .45f;
                        break;
                    }
                    case 9: { // checker implosion
                        int cell = (((int)x >> 5) + ((int)y >> 5)) & 1;
                        float direction = cell ? 1.0f : -1.0f;
                        sx_float += direction * ripple + fast_sin(phase + y * .04f) * 5.0f;
                        sy_float -= direction * ripple + fast_sin(phase - x * .04f) * 5.0f;
                        break;
                    }
                    case 10: // waterfall: treble ripples sideways, bass pulls down
                        sx_float += fast_sin(y * .027f + phase * 2.2f) * preset->ripple * (.35f + audio->treble);
                        sy_float -= preset->ripple * (.18f + audio->bass * 1.4f) + field * 4.0f;
                        break;
                    case 11: // laser smear
                        sx_float += field * preset->ripple * (1.0f + audio->treble * 2.0f);
                        sy_float += fast_sin(dx * .055f + phase) * 3.0f;
                        break;
                    case 12: { // binary suns: two orbiting gravity wells
                        float orbit_x = fast_sin(phase * .8f + TWO_PI * .25f) * screen_width * .22f;
                        float orbit_y = fast_sin(phase * .8f) * screen_height * .16f;
                        float d1 = (dx - orbit_x) * (dx - orbit_x) + (dy - orbit_y) * (dy - orbit_y) + 500.0f;
                        float d2 = (dx + orbit_x) * (dx + orbit_x) + (dy + orbit_y) * (dy + orbit_y) + 500.0f;
                        sx_float += preset->ripple * 900.0f * ((dx - orbit_x) / d1 - (dx + orbit_x) / d2);
                        sy_float += preset->ripple * 900.0f * ((dy - orbit_y) / d1 - (dy + orbit_y) / d2);
                        break;
                    }
                    case 13: { // spiral orchid petals
                        float petals = fast_sin(angle * preset->angular_frequency - phase * 2.0f);
                        float scale = 1.0f + petals * preset->ripple / (radius + 55.0f);
                        float twist = petals * .035f + audio->bass * .018f;
                        sx_float = cx + fast_sin(angle + twist + TWO_PI * .25f) * radius * scale;
                        sy_float = cy + fast_sin(angle + twist) * radius * scale;
                        break;
                    }
                    case 14: { // diamond / Manhattan fold
                        float diamond = fabsf(dx) + fabsf(dy) + 1.0f;
                        float pulse = fast_sin(diamond * preset->radial_frequency - phase * 2.5f);
                        sx_float = cx + rx * (1.0f + pulse * .045f) + (dx >= 0 ? ripple : -ripple);
                        sy_float = cy + ry * (1.0f - pulse * .045f) + (dy >= 0 ? ripple : -ripple);
                        break;
                    }
                    case 15: { // elastic ellipse
                        float stretch = 1.0f + fast_sin(phase * 1.3f + radius * .008f) * (.08f + audio->bass * .10f);
                        sx_float = cx + rx * stretch;
                        sy_float = cy + ry / stretch + ripple;
                        break;
                    }
                    case 16: { // per-band audio spokes
                        int band = ((int)((angle + 3.1415927f) * 2.55f)) % AUDIO_ANALYSIS_BANDS;
                        float power = audio->bands[band] * preset->ripple;
                        sx_float += dx / (radius + 8.0f) * power;
                        sy_float += dy / (radius + 8.0f) * power;
                        break;
                    }
                    case 17: { // zigzag / sawtooth field
                        int stripe = ((int)(x + y + phase * 35.0f) >> 4) & 3;
                        float zig = (stripe - 1.5f) * preset->ripple * .34f;
                        sx_float += zig; sy_float -= zig * (0.4f + audio->mid);
                        break;
                    }
                    case 18: { // orbit pull with a moving focal point
                        float ox = fast_sin(phase * .9f + TWO_PI * .25f) * screen_width * .28f;
                        float oy = fast_sin(phase * 1.1f) * screen_height * .22f;
                        float qx = dx - ox, qy = dy - oy;
                        float gravity = preset->ripple * 14.0f / (fabsf(qx) + fabsf(qy) + 30.0f);
                        sx_float -= qx * gravity; sy_float -= qy * gravity;
                        break;
                    }
                    case 19: { // logarithmic-feeling lava twist
                        float twist = fast_sin(phase + radius * .006f) * preset->ripple / (radius + 20.0f);
                        sx_float = cx + fast_sin(angle + twist + TWO_PI * .25f) * radius * zoom;
                        sy_float = cy + fast_sin(angle + twist) * radius * zoom;
                        break;
                    }
                    case 20: { // two-dimensional interference fabric
                        float a = fast_sin((dx + dy) * .025f + phase * 1.7f);
                        float b = fast_sin((dx - dy) * .031f - phase * 1.2f);
                        sx_float += a * preset->ripple; sy_float += b * preset->ripple;
                        break;
                    }
                    case 21: { // ring accelerator
                        float ring = fast_sin(radius * preset->radial_frequency - phase * 4.0f);
                        float kick = ring * preset->ripple * (.4f + audio->bass * 1.5f);
                        sx_float += dx / (radius + 5.0f) * kick;
                        sy_float += dy / (radius + 5.0f) * kick;
                        break;
                    }
                    case 22: { // beat cannon: aggressive zoom pulse
                        float kick = audio->beat_strength * .12f + audio->peak * .035f;
                        float scale = 1.0f - kick + field * .018f;
                        sx_float = cx + rx * scale; sy_float = cy + ry * scale;
                        break;
                    }
                    case 23: { // rotating mirrored tiles
                        int tile_x = (int)x >> 6, tile_y = (int)y >> 6;
                        float flip_x = ((tile_x + tile_y) & 1) ? -1.0f : 1.0f;
                        float flip_y = (tile_y & 1) ? -1.0f : 1.0f;
                        sx_float = cx + rx * flip_x + fast_sin(phase + tile_y) * preset->ripple;
                        sy_float = cy + ry * flip_y + fast_sin(phase - tile_x) * preset->ripple;
                        break;
                    }
                }
                int sx = (int)sx_float;
                int sy = (int)sy_float;
                if (sx < 0) sx = 0; else if (sx >= (int)screen_width) sx = screen_width - 1;
                if (sy < 0) sy = 0; else if (sy >= (int)screen_height) sy = screen_height - 1;
                if (overlay_visible && sx >= CARD_X && sx < CARD_X + CARD_W && sy >= CARD_Y && sy < CARD_Y + CARD_H) {
                    color = 0xff02040a;
                } else {
                    uint16_t raw = previous_buffer->buf_16bpp[sx * previous_buffer->width +
                                                              (previous_buffer->width - 1 - sy)];
                    pax_col_t old = previous_buffer->buf2col(previous_buffer, raw);
                    float decay = preset->decay + audio->rms * 0.018f;
                    int red = ((old >> 16) & 0xff) * decay;
                    int green = ((old >> 8) & 0xff) * decay;
                    int blue = (old & 0xff) * decay;
                    float crest = clamp01(field * 0.5f + 0.5f);
                    int injection = (int)((preset->injection + audio->bass * 10.0f +
                                           audio->beat_strength * 15.0f) * crest);
                    float color_phase = (hue + preset->hue_offset + radius * .08f + angle * 18.0f) * .0246f;
                    milk_palette(preset->palette, color_phase, injection, &red, &green, &blue);
                    color = pax_col_rgb(red > 255 ? 255 : red, green > 255 ? 255 : green, blue > 255 ? 255 : blue);
                }
            }
            fill_effect_cell(buffer, x, y, color);
        }
    }
    hue += (uint8_t)(dt * (3.0f + audio->treble * 5.0f));
    if (audio->beat_counter != last_beat) { hue += 2; last_beat = audio->beat_counter; }
}

static pax_col_t procedural_palette(float position, float accent) {
    return global_palette_color(position, accent);
}

static void seed_reaction(size_t cx, size_t cy, size_t radius) {
    if (!reaction_b) return;
    for (int y = (int)cy - (int)radius; y <= (int)cy + (int)radius; y++) {
        for (int x = (int)cx - (int)radius; x <= (int)cx + (int)radius; x++) {
            if (x > 0 && y > 0 && x < (int)grid_width - 1 && y < (int)grid_height - 1) {
                reaction_b[(size_t)y * grid_width + x] = .92f;
            }
        }
    }
}

static void render_reaction(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt,
                            effect_id_t effect) {
    (void)dt;
    if (!reaction_a || !reaction_b || !reaction_next_a || !reaction_next_b) return;
    if (previous_effect != effect) {
        size_t cells = grid_width * grid_height;
        for (size_t i = 0; i < cells; i++) {
            reaction_a[i] = reaction_next_a[i] = 1.0f;
            reaction_b[i] = reaction_next_b[i] = 0.0f;
        }
        seed_reaction(grid_width / 2, grid_height / 2, 4);
        seed_reaction(grid_width / 3, grid_height / 3, 3);
        seed_reaction(grid_width * 2 / 3, grid_height * 2 / 3, 3);
    }
    if (audio->beat_counter != last_beat) {
        size_t bx = 4 + (audio->beat_counter * 37u) % (grid_width - 8);
        size_t by = 4 + (audio->beat_counter * 23u) % (grid_height - 8);
        seed_reaction(bx, by, 2 + (size_t)(audio->beat_strength * 3.0f));
        last_beat = audio->beat_counter;
        hue += 11;
    }
    float feed = .035f + audio->bass * .012f;
    float kill = .060f + audio->treble * .010f;
    for (size_t y = 1; y + 1 < grid_height; y++) {
        for (size_t x = 1; x + 1 < grid_width; x++) {
            size_t i = y * grid_width + x;
            float a = reaction_a[i], b = reaction_b[i];
            float lap_a = reaction_a[i - 1] + reaction_a[i + 1] +
                          reaction_a[i - grid_width] + reaction_a[i + grid_width] - 4.0f * a;
            float lap_b = reaction_b[i - 1] + reaction_b[i + 1] +
                          reaction_b[i - grid_width] + reaction_b[i + grid_width] - 4.0f * b;
            float reaction = a * b * b;
            reaction_next_a[i] = clamp01(a + .82f * lap_a - reaction + feed * (1.0f - a));
            reaction_next_b[i] = clamp01(b + .42f * lap_b + reaction - (kill + feed) * b);
        }
    }
    float *swap = reaction_a; reaction_a = reaction_next_a; reaction_next_a = swap;
    swap = reaction_b; reaction_b = reaction_next_b; reaction_next_b = swap;
    for (size_t gy = 0; gy < grid_height; gy++) {
        for (size_t gx = 0; gx < grid_width; gx++) {
            float value = clamp01((reaction_a[gy * grid_width + gx] - reaction_b[gy * grid_width + gx]) * .9f + .15f);
            fill_effect_cell(buffer, gx * EFFECT_PIXEL, gy * EFFECT_PIXEL,
                             procedural_palette(value, audio->beat_strength));
        }
    }
}

static inline float pseudo_cell(float x, float y) {
    return fast_sin(x * 12.9898f + y * 78.233f) * .5f + .5f;
}

static void render_mind_ca(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt,
                           effect_id_t effect) {
    if (!mind_cells || !mind_next) return;
    size_t cells = grid_width * grid_height;
    if (previous_effect != effect) {
        memset(mind_cells, 0, cells);
        memset(mind_next, 0, cells);
        mind_cells[grid_width / 2] = 0x81;
        mind_cells[grid_width / 3] = 0x18;
        mind_step_time = 0;
        mind_last_beat = audio->beat_counter;
    }
    if (audio->beat_counter != mind_last_beat) {
        mind_lfsr = mind_lfsr * 1664525u + 1013904223u;
        size_t position = (mind_lfsr >> 16) % grid_width;
        mind_cells[position] ^= (uint8_t)(1u << (audio->beat_counter & 7));
        mind_last_beat = audio->beat_counter;
    }

    mind_step_time += dt;
    float step_interval = .085f - audio->bass * .030f;
    int steps = 0;
    while (mind_step_time >= step_interval && steps++ < 3) {
        mind_step_time -= step_interval;
        memcpy(mind_next, mind_cells, grid_width);
        for (size_t y = 1; y < grid_height; y++) {
            size_t source = (y - 1) * grid_width;
            size_t destination = y * grid_width;
            for (size_t x = 0; x < grid_width; x++) {
                uint8_t left = mind_cells[source + (x + grid_width - 1) % grid_width];
                uint8_t center = mind_cells[source + x];
                uint8_t right = mind_cells[source + (x + 1) % grid_width];
                uint8_t grown = left ^ right;
                if (audio->mid > .42f) grown ^= (uint8_t)((center << 1) | (center >> 7));
                if (audio->treble > .68f && ((x + y + audio->beat_counter) & 31u) == 0) {
                    grown ^= (uint8_t)(mind_lfsr >> ((x + y) & 15));
                }
                mind_next[destination + x] = grown;
            }
        }
        uint8_t *swap = mind_cells; mind_cells = mind_next; mind_next = swap;
    }

    for (size_t gy = 0; gy < grid_height; gy++) {
        for (size_t gx = 0; gx < grid_width; gx++) {
            uint8_t cell = mind_cells[gy * grid_width + gx];
            float density = __builtin_popcount((unsigned)cell) / 8.0f;
            float value = cell ? .24f + density * .70f + audio->beat_strength * .12f : .015f;
            float structure = ((cell >> ((gx + gy) & 7)) & 1) ? .14f : 0.0f;
            fill_effect_cell(buffer, gx * EFFECT_PIXEL, gy * EFFECT_PIXEL,
                             global_palette_color(clamp01(value + structure), audio->beat_strength));
        }
    }
}

static void render_procedural(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt,
                              effect_id_t effect) {
    int kind = effect - EFFECT_PROCEDURAL_FIRST;
    if (kind == 0) { render_reaction(buffer, audio, dt, effect); return; }
    if (kind == 48) { render_mind_ca(buffer, audio, dt, effect); return; }
    phase += dt * (.65f + audio->mid * 1.8f);
    if (audio->beat_counter != last_beat) { hue += 17; last_beat = audio->beat_counter; }
    float cx = screen_width * .5f, cy = screen_height * .5f;
    float infinite_scale = powf(2.0f, fmodf(phase * .32f, 1.0f));
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            size_t index = (y / EFFECT_PIXEL) * grid_width + x / EFFECT_PIXEL;
            float radius = radial_lut ? radial_lut[index] : 0.0f;
            float angle = angle_lut ? angle_lut[index] : 0.0f;
            float nx = ((float)x - cx) / (screen_height * .5f);
            float ny = ((float)y - cy) / (screen_height * .5f);
            float value = 0.0f;
            float tint = 0.0f;
            switch (kind) {
                case 1: { // Voronoi lava
                    float nearest = 9999.0f, second = 9999.0f;
                    int winner = 0;
                    for (int p = 0; p < 6; p++) {
                        float px = cx + fast_sin(phase * (.45f + p * .07f) + p * 1.9f) * screen_width * .43f;
                        float py = cy + fast_sin(phase * (.57f + p * .05f) + p * 2.7f) * screen_height * .40f;
                        float dx = x - px, dy = y - py;
                        float d = dx * dx + dy * dy;
                        if (d < nearest) { second = nearest; nearest = d; winner = p; }
                        else if (d < second) second = d;
                    }
                    float edge = clamp01((second - nearest) / 4800.0f);
                    value = clamp01(.18f + edge * .8f + audio->bass * .22f);
                    tint = winner * 35.0f + nearest * .0015f;
                    break;
                }
                case 2: { // Julia dream
                    float zx = nx * 1.55f, zy = ny * 1.55f;
                    float cr = -.72f + fast_sin(phase * .31f) * .12f + audio->bass * .08f;
                    float ci = .25f + fast_sin(phase * .23f + 1.0f) * .18f;
                    int iter = 0;
                    for (; iter < 13 && zx * zx + zy * zy < 4.0f; iter++) {
                        float next = zx * zx - zy * zy + cr;
                        zy = 2.0f * zx * zy + ci; zx = next;
                    }
                    value = iter == 13 ? .05f + audio->rms * .2f : iter / 13.0f;
                    tint = iter * 17.0f + phase * 16.0f;
                    break;
                }
                case 3: { // Mandelbrot pulse
                    float zoom = 1.25f - audio->bass * .38f;
                    float cr = nx * 2.25f * zoom - .55f + fast_sin(phase * .18f) * .08f;
                    float ci = ny * 2.25f * zoom;
                    float zx = 0, zy = 0;
                    int iter = 0;
                    for (; iter < 12 && zx * zx + zy * zy < 4.0f; iter++) {
                        float next = zx * zx - zy * zy + cr;
                        zy = 2.0f * zx * zy + ci; zx = next;
                    }
                    value = iter == 12 ? audio->beat_strength * .45f : iter / 12.0f;
                    tint = iter * 21.0f;
                    break;
                }
                case 4: { // Moire silk
                    float d1 = sqrtf((x - cx * .55f) * (x - cx * .55f) + (y - cy) * (y - cy));
                    float d2 = sqrtf((x - cx * 1.45f) * (x - cx * 1.45f) + (y - cy) * (y - cy));
                    float weave = fast_sin(d1 * (.055f + audio->treble * .03f) + phase * 2.0f) +
                                  fast_sin(d2 * (.052f + audio->mid * .025f) - phase * 1.7f);
                    value = clamp01(.5f + weave * .24f + audio->beat_strength * .2f);
                    tint = (d1 - d2) * .35f;
                    break;
                }
                case 5: { // Lissajous field
                    float a = fast_sin(x * .024f + phase * 1.8f + audio->bass * 2.0f);
                    float b = fast_sin(y * .037f - phase * 1.3f + audio->treble * 3.0f);
                    float c = fast_sin((x + y) * .014f + phase * .7f);
                    value = clamp01(.45f + (a * b + c) * .28f);
                    tint = a * 70.0f + b * 45.0f;
                    break;
                }
                case 6: { // Audio mosaic
                    int band = ((int)x * AUDIO_ANALYSIS_BANDS / (int)screen_width) % AUDIO_ANALYSIS_BANDS;
                    int tile = (((int)x >> 5) ^ ((int)y >> 5)) & 7;
                    float power = audio->bands[band];
                    value = clamp01(.08f + power * .88f + (tile == (audio->beat_counter & 7) ? audio->beat_strength * .5f : 0));
                    tint = band * 16.0f + tile * 24.0f;
                    break;
                }
                case 7: { // Polar mandala
                    float petals = fast_sin(angle * (6.0f + (int)(audio->treble * 7.0f)) + phase * 1.5f);
                    float rings = fast_sin(radius * (.035f + audio->bass * .025f) - phase * 2.7f);
                    value = clamp01(.48f + petals * rings * .47f + audio->beat_strength * .18f);
                    tint = angle * 42.0f + radius * .18f;
                    break;
                }
                case 8: { // Checker abyss
                    int ring = (int)(1900.0f / (radius + 18.0f) + phase * 4.0f);
                    int spoke = (int)((angle + 3.1415927f) * (5.0f + audio->mid * 7.0f));
                    bool white = (ring ^ spoke) & 1;
                    value = white ? .92f : .08f + audio->bass * .25f;
                    tint = ring * 13.0f + spoke * 21.0f;
                    break;
                }
                case 9: { // Cellular flame
                    float warp_x = nx + fast_sin(ny * 9.0f - phase * 2.0f) * .15f;
                    float warp_y = ny + fast_sin(nx * 11.0f + phase) * .12f;
                    float cells = pseudo_cell(floorf(warp_x * 8.0f), floorf(warp_y * 8.0f));
                    float flame = fast_sin((warp_x * warp_x + warp_y * warp_y) * 13.0f - phase * 3.0f);
                    value = clamp01(.25f + cells * .35f + flame * .28f + audio->bass * .25f);
                    tint = 8.0f + cells * 55.0f;
                    break;
                }
                case 10: { // Bass ripples
                    float drop1 = fast_sin(radius * .060f - phase * 3.4f);
                    float dx = x - cx - fast_sin(phase) * 160.0f;
                    float dy = y - cy - fast_sin(phase * .7f) * 110.0f;
                    float drop2 = fast_sin(sqrtf(dx * dx + dy * dy) * .075f + phase * 2.2f);
                    value = clamp01(.44f + drop1 * drop2 * (.25f + audio->bass * .35f));
                    tint = radius * .28f + drop2 * 50.0f;
                    break;
                }
                case 11: { // Spectral crystal
                    float diamond = fabsf(nx) + fabsf(ny);
                    int band = ((int)(fabsf(angle) * 5.1f)) % AUDIO_ANALYSIS_BANDS;
                    float facets = fabsf(fast_sin(diamond * 18.0f - phase * 2.0f));
                    value = clamp01(.12f + facets * .55f + audio->bands[band] * .55f);
                    tint = band * 19.0f + diamond * 90.0f;
                    break;
                }
                case 12: { // Velvet clouds: layered domain noise
                    float cloud = fast_sin(nx * 5.0f + phase) * .50f;
                    cloud += fast_sin(ny * 8.0f - phase * .7f + cloud) * .28f;
                    cloud += fast_sin((nx + ny) * 13.0f + phase * 1.3f) * .15f;
                    value = clamp01(.43f + cloud * .42f + audio->rms * .22f);
                    tint = cloud * 40.0f;
                    break;
                }
                case 13: { // Superformula-inspired living radial creature
                    float lobes = 3.0f + (int)(audio->mid * 7.0f);
                    float boundary = 150.0f + fast_sin(angle * lobes + phase) *
                                     (52.0f + audio->bass * 85.0f);
                    float edge = fabsf(radius - boundary);
                    value = clamp01(1.0f - edge / 42.0f + fast_sin(radius * .08f - phase * 2.0f) * .18f);
                    tint = boundary * .22f;
                    break;
                }
                case 14: { // Domain-warped marble veins
                    float wx = nx + fast_sin(ny * 7.0f + phase) * (.22f + audio->bass * .18f);
                    float wy = ny + fast_sin(nx * 9.0f - phase * .8f) * (.18f + audio->mid * .16f);
                    float vein = fast_sin((wx * 5.0f + wy * 11.0f) + fast_sin(wy * 15.0f) * 2.4f);
                    value = clamp01(.48f + vein * .44f + audio->beat_strength * .12f);
                    tint = vein * 25.0f;
                    break;
                }
                case 15: { // Truchet circuit tiles
                    int tx = (int)x >> 5, ty = (int)y >> 5;
                    float lx = fmodf((float)x, 32.0f) - 16.0f;
                    float ly = fmodf((float)y, 32.0f) - 16.0f;
                    bool flip = (((tx * 13 + ty * 7) ^ (int)(phase * .7f)) & 1) != 0;
                    float qx = lx + (flip ? 16.0f : -16.0f);
                    float qy = ly - 16.0f;
                    float arc = fabsf(sqrtf(qx * qx + qy * qy) - 16.0f);
                    value = clamp01(1.0f - arc / (2.2f + audio->treble * 4.0f));
                    tint = (tx + ty) * 7.0f;
                    break;
                }
                case 16: { // Hex-like pulsing reactor cells
                    float row = floorf(y / 42.0f);
                    float hx = fmodf(x + (fmodf(row, 2.0f) * 24.0f), 48.0f) - 24.0f;
                    float hy = fmodf(y, 42.0f) - 21.0f;
                    float hex = fmaxf(fabsf(hx) * .866f + fabsf(hy) * .5f, fabsf(hy));
                    float pulse = fast_sin(phase * 2.0f + row + floorf(x / 48.0f));
                    value = clamp01(1.0f - fabsf(hex - 15.0f - pulse * audio->bass * 5.0f) / 7.0f);
                    tint = row * 9.0f + pulse * 18.0f;
                    break;
                }
                case 17: { // Magnetic flux between two poles
                    float separation = 125.0f + audio->bass * 65.0f;
                    float a1 = atan2f(y - cy, x - (cx - separation));
                    float a2 = atan2f(y - cy, x - (cx + separation));
                    float flux = fast_sin((a1 - a2) * (9.0f + audio->treble * 8.0f) + phase * 2.0f);
                    value = clamp01(.48f + flux * .46f);
                    tint = (a1 + a2) * 25.0f;
                    break;
                }
                case 18: { // Rose curve engine
                    float petals = 5.0f + (int)(audio->treble * 5.0f);
                    float target = fabsf(fast_sin(angle * petals + phase * .8f)) *
                                   (190.0f + audio->bass * 70.0f);
                    float edge = fabsf(radius - target);
                    value = clamp01(1.0f - edge / 25.0f + audio->beat_strength * .18f);
                    tint = angle * 18.0f + target * .15f;
                    break;
                }
                case 19: { // Ocean caustic interference
                    float caustic = fast_sin(nx * 14.0f + fast_sin(ny * 8.0f + phase) * 2.0f);
                    caustic *= fast_sin(ny * 17.0f + fast_sin(nx * 10.0f - phase * 1.2f) * 2.5f);
                    value = clamp01(.38f + fabsf(caustic) * .62f + audio->treble * .12f);
                    tint = caustic * 22.0f;
                    break;
                }
                case 20: { // Synthwave terrain layers
                    float horizon = cy + 55.0f + fast_sin(x * .017f + phase) * (35.0f + audio->bass * 60.0f);
                    float layer = fmodf(fabsf(y - horizon) + phase * 25.0f, 44.0f);
                    float ridge = 1.0f - fminf(layer, 44.0f - layer) / 22.0f;
                    value = y < horizon ? .04f + audio->treble * .12f : clamp01(ridge * .85f + audio->beat_strength * .25f);
                    tint = (y - horizon) * .35f;
                    break;
                }
                case 21: { // Interference bead lattice
                    float gx = fast_sin(x * .052f + phase * 1.4f);
                    float gy = fast_sin(y * .061f - phase * 1.1f);
                    float bead = gx * gx + gy * gy;
                    float threshold = .20f + audio->mid * .45f;
                    value = clamp01(1.0f - fabsf(bead - threshold) * 2.2f);
                    tint = (gx - gy) * 30.0f;
                    break;
                }
                case 22: { // Plasma colonies / independent metaball field
                    float field_sum = 0.0f;
                    for (int p = 0; p < 5; p++) {
                        float px = cx + fast_sin(phase * (.6f + p * .09f) + p) * 300.0f;
                        float py = cy + fast_sin(phase * (.5f + p * .11f) + p * 2.1f) * 180.0f;
                        float dx = x - px, dy = y - py;
                        field_sum += (2100.0f + audio->bass * 3200.0f) / (dx * dx + dy * dy + 300.0f);
                    }
                    value = clamp01((field_sum - .20f) * 1.35f);
                    tint = field_sum * 35.0f;
                    break;
                }
                case 23: { // Recursive concentric squares
                    float square_radius = fmaxf(fabsf(x - cx), fabsf(y - cy));
                    float ring = fast_sin(square_radius * (.075f + audio->mid * .035f) - phase * 3.0f);
                    float diagonal = fast_sin((fabsf(nx) + fabsf(ny)) * 12.0f + phase);
                    value = clamp01(.46f + ring * diagonal * .45f + audio->beat_strength * .16f);
                    tint = square_radius * .22f;
                    break;
                }
                case 24: { // Infinite folding fractal zoom
                    float fx = nx * infinite_scale;
                    float fy = ny * infinite_scale;
                    for (int fold = 0; fold < 5; fold++) {
                        fx = fabsf(fx) * 1.72f - .83f + fast_sin(phase * .13f) * .04f;
                        fy = fabsf(fy) * 1.72f - .68f;
                        float rotated = fx * .82f - fy * .57f;
                        fy = fx * .57f + fy * .82f;
                        fx = rotated;
                    }
                    float form = fast_sin((fabsf(fx) + fabsf(fy)) * 12.0f - phase * 2.0f);
                    value = clamp01(.48f + form * .44f + audio->bass * .18f);
                    tint = (fx - fy) * 42.0f;
                    break;
                }
                case 25: { // Vector vortex flow magnitude
                    float spin = angle + phase * .35f + fast_sin(radius * .018f - phase) *
                                 (.45f + audio->bass * .65f);
                    float vx = fast_sin(spin + TWO_PI * .25f) * fast_sin(ny * 9.0f + phase);
                    float vy = fast_sin(spin) * fast_sin(nx * 11.0f - phase * .8f);
                    float curl = fast_sin((vx - vy) * 8.0f + radius * .035f);
                    value = clamp01(.46f + curl * (.32f + audio->mid * .22f));
                    tint = spin * 28.0f;
                    break;
                }
                case 26: { // Strange implicit geometry / attractor field
                    float sx = fast_sin(nx * (7.0f + audio->bass * 5.0f) + fast_sin(ny * 5.0f + phase));
                    float sy = fast_sin(ny * (9.0f + audio->treble * 6.0f) - fast_sin(nx * 6.0f - phase));
                    float attractor = sx * sy + fast_sin((sx - sy) * 5.0f + phase * 1.4f) * .45f;
                    value = clamp01(.46f + attractor * .38f + audio->beat_strength * .16f);
                    tint = (sx + sy) * 38.0f;
                    break;
                }
                case 27: { // Infinite nested portals
                    float px = nx, py = ny;
                    float portal = 0.0f;
                    for (int level = 0; level < 4; level++) {
                        float local_radius = sqrtf(px * px + py * py);
                        portal += fast_sin(local_radius * (18.0f + level * 7.0f) - phase * (2.0f + level * .3f)) /
                                  (level + 1.0f);
                        px = fabsf(px) * 1.85f - .62f;
                        py = fabsf(py) * 1.85f - .62f;
                    }
                    value = clamp01(.47f + portal * .24f + audio->bass * .14f);
                    tint = portal * 34.0f;
                    break;
                }
                case 28: { // Four restrained, breathing planes
                    int region = (x >= cx ? 1 : 0) + (y >= cy ? 2 : 0);
                    static const float levels[4] = {.08f, .34f, .62f, .88f};
                    value = clamp01(levels[region] + audio->bands[region * 3] * .22f);
                    tint = region * 18.0f;
                    break;
                }
                case 29: { // A single bass-driven monolith
                    float half_width = 58.0f + audio->bass * 125.0f;
                    bool inside = fabsf(x - cx) < half_width && y > 46 && y < screen_height - 38;
                    value = inside ? .62f + audio->peak * .35f : .015f;
                    if (inside && fabsf(x - cx) < half_width * .18f) value = .96f;
                    tint = inside ? 20.0f : 0.0f;
                    break;
                }
                case 30: { // Quiet three-band horizon
                    float horizon = cy + fast_sin(phase * .32f) * 42.0f;
                    if (y < horizon - 75.0f) value = .06f + audio->treble * .10f;
                    else if (y < horizon) value = .34f + audio->mid * .20f;
                    else value = .72f + audio->bass * .24f;
                    tint = y < horizon ? 8.0f : 26.0f;
                    break;
                }
                case 31: { // Bauhaus blocks with beat-selected emphasis
                    int column = (int)x * 5 / (int)screen_width;
                    int row = (int)y * 3 / (int)screen_height;
                    int block = row * 5 + column;
                    bool active = block == (int)(audio->beat_counter % 15);
                    value = active ? .98f : ((block * 7) % 5) * .16f + .08f;
                    tint = block * 9.0f;
                    break;
                }
                case 32: { // Diagonal negative-space split
                    float boundary = screen_height * .28f + x * .34f + fast_sin(phase) * 55.0f;
                    float distance = y - boundary;
                    value = distance > 18 ? .03f : (distance > -24 ? .94f : .23f + audio->bass * .25f);
                    tint = distance > 0 ? 0.0f : 30.0f;
                    break;
                }
                case 33: { // Two large orbiting colour fields
                    float ox = fast_sin(phase * .55f + TWO_PI * .25f) * 180.0f;
                    float oy = fast_sin(phase * .55f) * 95.0f;
                    float d1 = sqrtf((x - cx - ox) * (x - cx - ox) + (y - cy - oy) * (y - cy - oy));
                    float d2 = sqrtf((x - cx + ox) * (x - cx + ox) + (y - cy + oy) * (y - cy + oy));
                    float radius1 = 70.0f + audio->bass * 90.0f;
                    value = d1 < radius1 ? .88f : (d2 < 105.0f + audio->mid * 55.0f ? .48f : .025f);
                    tint = d1 < radius1 ? 8.0f : 30.0f;
                    break;
                }
                case 34: { // Mostly empty; one moving luminous slab
                    float slab_x = cx + fast_sin(phase * .38f) * 260.0f;
                    float slab_y = cy + fast_sin(phase * .29f + 1.0f) * 120.0f;
                    bool slab = fabsf(x - slab_x) < 52.0f && fabsf(y - slab_y) < 145.0f;
                    value = slab ? .68f + audio->rms * .30f : .008f;
                    tint = slab ? 18.0f : 0.0f;
                    break;
                }
                case 35: { // Wide spectral stripes, no rainbow
                    int stripe = (int)x * AUDIO_ANALYSIS_BANDS / (int)screen_width;
                    float power = audio->bands[stripe];
                    bool gap = ((int)x % 50) < 7;
                    value = gap ? .015f : .12f + power * .84f;
                    tint = stripe * 3.0f;
                    break;
                }
                case 36: { // Mondrian-like large rectangles
                    int column = x < screen_width * .24f ? 0 : (x < screen_width * .70f ? 1 : 2);
                    int row = y < screen_height * .38f ? 0 : (y < screen_height * .76f ? 1 : 2);
                    bool border = abs((int)x - (int)(screen_width * .24f)) < 7 ||
                                  abs((int)x - (int)(screen_width * .70f)) < 7 ||
                                  abs((int)y - (int)(screen_height * .38f)) < 7 ||
                                  abs((int)y - (int)(screen_height * .76f)) < 7;
                    int cell = row * 3 + column;
                    value = border ? .01f : .18f + ((cell * 5 + audio->beat_counter) % 4) * .22f;
                    tint = cell * 12.0f;
                    break;
                }
                case 37: { // Four asymmetrical colour blocks
                    bool left = x < screen_width * (.32f + audio->bass * .12f);
                    bool top = y < screen_height * (.58f - audio->mid * .15f);
                    int block = (left ? 0 : 1) + (top ? 0 : 2);
                    static const float block_values[4] = {.12f, .78f, .46f, .92f};
                    value = clamp01(block_values[block] + audio->beat_strength * .12f);
                    tint = block * 16.0f;
                    break;
                }
                case 38: { // Sparse tunnel: thin rings in darkness
                    float ring = fmodf(radius + phase * (38.0f + audio->bass * 45.0f), 82.0f);
                    float edge = fminf(ring, 82.0f - ring);
                    value = edge < 7.0f ? .82f + audio->treble * .18f : .012f;
                    tint = radius * .08f;
                    break;
                }
                case 39: { // Two flat suns
                    float separation = 135.0f + fast_sin(phase * .4f) * 55.0f;
                    float d1 = (x - cx - separation) * (x - cx - separation) + (y - cy) * (y - cy);
                    float d2 = (x - cx + separation) * (x - cx + separation) + (y - cy) * (y - cy);
                    float sun = (72.0f + audio->bass * 55.0f); sun *= sun;
                    value = d1 < sun ? .94f : (d2 < sun ? .55f : .018f);
                    tint = d1 < sun ? 7.0f : 29.0f;
                    break;
                }
                case 40: { // Julia folding mixed with caustics
                    float fx = nx, fy = ny;
                    for (int i = 0; i < 5; i++) {
                        float next = fx * fx - fy * fy - .62f;
                        fy = 2.0f * fx * fy + .31f; fx = next;
                    }
                    float caustic = fast_sin(fx * 9.0f + phase) * fast_sin(fy * 11.0f - phase);
                    value = clamp01(.44f + caustic * .42f + audio->bass * .20f);
                    tint = (fx + fy) * 22.0f;
                    break;
                }
                case 41: { // Voronoi distance cut by checker planes
                    float nearest = 99999.0f;
                    for (int p = 0; p < 4; p++) {
                        float px = cx + fast_sin(phase * .5f + p * 1.7f) * 310.0f;
                        float py = cy + fast_sin(phase * .7f + p * 2.3f) * 190.0f;
                        float dx = x - px, dy = y - py;
                        nearest = fminf(nearest, dx * dx + dy * dy);
                    }
                    bool checker = ((((int)x >> 5) ^ ((int)y >> 5)) & 1) != 0;
                    value = clamp01((checker ? .28f : .72f) + fast_sin(nearest * .0008f - phase) * .25f);
                    tint = nearest * .001f;
                    break;
                }
                case 42: { // Magnetic field lines inside a mandala
                    float poles = atan2f(y - cy, x - cx - 120.0f) - atan2f(y - cy, x - cx + 120.0f);
                    float petals = fast_sin(angle * (7.0f + audio->treble * 6.0f) + phase);
                    value = clamp01(.48f + fast_sin(poles * 9.0f + petals * 2.0f) * .44f);
                    tint = petals * 35.0f;
                    break;
                }
                case 43: { // Marble veins constrained to circuit tiles
                    float marble = fast_sin(nx * 10.0f + fast_sin(ny * 13.0f + phase) * 2.8f);
                    int tile = (((int)x >> 5) + ((int)y >> 5)) & 1;
                    value = clamp01(.46f + marble * (tile ? .42f : .18f) + audio->mid * .16f);
                    tint = marble * 28.0f + tile * 20.0f;
                    break;
                }
                case 44: { // Crystalline facets twisted as a vortex
                    float twist = angle + fast_sin(radius * .02f - phase) * (.6f + audio->bass);
                    float crystal = fabsf(fast_sin((fabsf(nx) + fabsf(ny)) * 17.0f + twist * 6.0f));
                    value = clamp01(.08f + crystal * .82f + audio->treble * .14f);
                    tint = twist * 25.0f;
                    break;
                }
                case 45: { // Reaction-like spots inside rigid geometry
                    float chemistry = fast_sin(nx * 14.0f + fast_sin(ny * 9.0f - phase) * 3.0f);
                    float geometry = fast_sin((fabsf(nx) + fabsf(ny)) * 19.0f + phase);
                    value = clamp01(.43f + chemistry * geometry * .45f + audio->bass * .18f);
                    tint = chemistry * 32.0f;
                    break;
                }
                case 46: { // Audio mosaic imposed on a fractal iteration field
                    float fx = nx * 1.4f, fy = ny * 1.4f;
                    int iter = 0;
                    for (; iter < 8 && fx * fx + fy * fy < 4.0f; iter++) {
                        float next = fx * fx - fy * fy - .70f;
                        fy = 2.0f * fx * fy + .27f; fx = next;
                    }
                    int band = ((int)x * AUDIO_ANALYSIS_BANDS / (int)screen_width) % AUDIO_ANALYSIS_BANDS;
                    value = clamp01(iter / 8.0f * .62f + audio->bands[band] * .55f);
                    tint = iter * 11.0f + band * 4.0f;
                    break;
                }
                case 47: { // Infinite recursive flora
                    float flower = 0.0f;
                    float pr = radius / 240.0f;
                    for (int level = 1; level <= 4; level++) {
                        flower += fast_sin(angle * (level * 3.0f + 2.0f) + pr * level * 12.0f - phase * level) / level;
                    }
                    value = clamp01(.46f + flower * .27f + audio->beat_strength * .18f);
                    tint = flower * 38.0f;
                    break;
                }
                case 49: { // Amiga-style copper bars
                    float bar1 = fabsf(y - (cy + fast_sin(phase * .8f) * 145.0f));
                    float bar2 = fabsf(y - (cy + fast_sin(phase * 1.1f + 2.1f) * 125.0f));
                    float bar3 = fabsf(y - (cy + fast_sin(phase * .6f + 4.0f) * 170.0f));
                    float glow = fmaxf(0.0f, 1.0f - fminf(bar1, fminf(bar2, bar3)) /
                                       (24.0f + audio->bass * 28.0f));
                    value = .018f + glow * (.72f + audio->rms * .25f);
                    tint = (bar1 < bar2 ? bar1 : bar2) * .8f;
                    break;
                }
                case 50: { // Kefrens sine bars
                    float nearest = 999.0f;
                    for (int bar = 0; bar < 7; bar++) {
                        float bar_x = cx + fast_sin(y * (.018f + bar * .0015f) + phase *
                                                   (.65f + bar * .07f) + bar * .82f) *
                                                   (230.0f + audio->bass * 75.0f);
                        nearest = fminf(nearest, fabsf(x - bar_x));
                    }
                    value = nearest < 18.0f ? clamp01(.94f - nearest / 22.0f + audio->beat_strength * .18f) : .012f;
                    tint = nearest * 3.0f;
                    break;
                }
                case 51: { // Rotozoom checker tiles
                    float rotation = phase * .38f;
                    float cs = fast_sin(rotation + TWO_PI * .25f);
                    float sn = fast_sin(rotation);
                    float zoom = .035f + audio->bass * .018f;
                    float u = ((x - cx) * cs - (y - cy) * sn) * zoom;
                    float v = ((x - cx) * sn + (y - cy) * cs) * zoom;
                    int checker = ((int)floorf(u) ^ (int)floorf(v)) & 1;
                    float edge_u = fabsf(fmodf(fabsf(u), 1.0f) - .5f);
                    float edge_v = fabsf(fmodf(fabsf(v), 1.0f) - .5f);
                    value = checker ? .78f + audio->treble * .18f : .08f;
                    if (edge_u > .44f || edge_v > .44f) value = .98f;
                    tint = (u + v) * 5.0f;
                    break;
                }
                case 52: { // Shadebob accumulation field
                    float shade = 0.0f;
                    for (int bob = 0; bob < 9; bob++) {
                        float bx = cx + fast_sin(phase * (.38f + bob * .025f) + bob * .71f) * 310.0f;
                        float by = cy + fast_sin(phase * (.51f + bob * .021f) + bob * 1.19f) * 185.0f;
                        float dx = x - bx, dy = y - by;
                        shade += (1500.0f + audio->mid * 1200.0f) / (dx * dx + dy * dy + 650.0f);
                    }
                    value = clamp01(.02f + shade * .38f + audio->beat_strength * .10f);
                    tint = shade * 21.0f;
                    break;
                }
                default: { // Classic scene twister
                    float center = cx + fast_sin(y * .021f + phase * 1.2f) *
                                           (95.0f + audio->bass * 90.0f);
                    float width = 42.0f + (fast_sin(y * .032f - phase * 1.8f) * .5f + .5f) * 115.0f;
                    float local = (x - center) / width;
                    if (fabsf(local) > 1.0f) value = .012f;
                    else {
                        float face = fast_sin(local * 3.1415927f + phase * 1.5f);
                        value = clamp01(.34f + face * .42f + audio->treble * .22f);
                    }
                    tint = local * 30.0f;
                    break;
                }
            }
            float texture = fast_sin(tint * .0246f) * .5f + .5f;
            float texture_mix = (kind >= 28 && kind < 40) ? 0.0f : .18f;
            float palette_position = clamp01(value * (1.0f - texture_mix) + texture * texture_mix);
            fill_effect_cell(buffer, x, y,
                             procedural_palette(palette_position, audio->beat_strength));
        }
    }
}

static void render_algorithmic_morph(pax_buf_t *buffer) {
    if (!morph_start || !morph_pixels || !buffer->buf_16bpp) return;
    float progress = (esp_timer_get_time() - morph_start) / 3200000.0f;
    if (progress >= 1.0f) { morph_start = 0; return; }
    if (progress < 0.0f) progress = 0.0f;
    progress = progress * progress * (3.0f - 2.0f * progress);
    float cx = screen_width * .5f, cy = screen_height * .5f;
    float warp = fast_sin(progress * 3.1415927f);
    size_t raw_width = buffer->width;
    for (size_t y = 0; y < screen_height; y += EFFECT_PIXEL) {
        for (size_t x = 0; x < screen_width; x += EFFECT_PIXEL) {
            if (overlay_visible && x < CARD_X + CARD_W && x + EFFECT_PIXEL > CARD_X &&
                y < CARD_Y + CARD_H && y + EFFECT_PIXEL > CARD_Y) continue;
            size_t index = (y / EFFECT_PIXEL) * grid_width + x / EFFECT_PIXEL;
            float radius = radial_lut ? radial_lut[index] : 0.0f;
            float angle = angle_lut ? angle_lut[index] : 0.0f;
            float dx = x - cx, dy = y - cy;
            float flow_phase = morph_sequence * 1.37f;
            float flow_x = fast_sin(y * .026f + flow_phase + progress * 2.3f) +
                           fast_sin(radius * .018f - flow_phase) * .55f;
            float flow_y = fast_sin(x * .021f - flow_phase - progress * 1.9f) +
                           fast_sin(angle * 3.0f + progress * 2.0f) * .45f;
            float twist = warp * (.16f + fast_sin(radius * .012f + flow_phase) * .08f);
            float sa = fast_sin(twist), ca = fast_sin(twist + TWO_PI * .25f);
            float sx_float = cx + (dx * ca - dy * sa) * (1.0f + warp * .045f) + flow_x * warp * 24.0f;
            float sy_float = cy + (dx * sa + dy * ca) * (1.0f + warp * .045f) + flow_y * warp * 20.0f;
            int sx = (int)sx_float, sy = (int)sy_float;
            if (sx < 0) sx = 0; else if (sx >= (int)screen_width) sx = screen_width - 1;
            if (sy < 0) sy = 0; else if (sy >= (int)screen_height) sy = screen_height - 1;

            uint16_t old_raw = morph_pixels[(size_t)sx * raw_width + (raw_width - 1 - (size_t)sy)];
            size_t current_offset = x * raw_width + (raw_width - 1 - y);
            uint16_t new_raw = buffer->buf_16bpp[current_offset];
            pax_col_t old_color = buffer->buf2col(buffer, old_raw);
            pax_col_t new_color = buffer->buf2col(buffer, new_raw);
            float turbulence = (flow_x + flow_y) * .075f * warp;
            float mix = clamp01(progress + turbulence);
            mix = mix * mix * (3.0f - 2.0f * mix);
            int red = (int)(((old_color >> 16) & 255) * (1.0f - mix) +
                            ((new_color >> 16) & 255) * mix);
            int green = (int)(((old_color >> 8) & 255) * (1.0f - mix) +
                              ((new_color >> 8) & 255) * mix);
            int blue = (int)((old_color & 255) * (1.0f - mix) + (new_color & 255) * mix);
            fill_effect_cell(buffer, x, y, pax_col_rgb(red, green, blue));
        }
    }
}

void effects_render(pax_buf_t *buffer, const audio_analysis_snapshot_t *audio, float dt) {
    if (buffer == NULL || audio == NULL) return;
    audio_analysis_snapshot_t adjusted = *audio;
    float intensity = visual_intensity == 0 ? 0.68f : (visual_intensity == 2 ? 1.28f : 1.0f);
    adjusted.rms = clamp01(adjusted.rms * intensity);
    adjusted.peak = clamp01(adjusted.peak * intensity);
    adjusted.bass = clamp01(adjusted.bass * intensity);
    adjusted.mid = clamp01(adjusted.mid * intensity);
    adjusted.treble = clamp01(adjusted.treble * intensity);
    adjusted.beat_strength = clamp01(adjusted.beat_strength * intensity);
    for (size_t i = 0; i < AUDIO_ANALYSIS_BANDS; i++) adjusted.bands[i] = clamp01(adjusted.bands[i] * intensity);
    audio = &adjusted;
    update_global_palette(audio, dt);
    switch (current_effect) {
        case EFFECT_PLASMA: render_plasma(buffer, audio, dt); break;
        case EFFECT_METABALLS: render_metaballs(buffer, audio, dt); break;
        case EFFECT_SPECTRUM: render_spectrum(buffer, audio, dt); break;
        case EFFECT_SCOPE: render_scope(buffer, audio, dt); break;
        case EFFECT_TUNNEL: render_tunnel(buffer, audio, dt); break;
        case EFFECT_RADIAL: render_radial(buffer, audio, dt); break;
        case EFFECT_STARFIELD: render_starfield(buffer, audio, dt); break;
        case EFFECT_KALEIDO: render_kaleido(buffer, audio, dt); break;
        case EFFECT_FLUID: render_fluid(buffer, audio, dt); break;
        case EFFECT_RIBBONS: render_ribbons(buffer, audio, dt); break;
        default:
            if (current_effect >= EFFECT_MILKDROP_FIRST && current_effect < EFFECT_COUNT) {
                if (current_effect < EFFECT_PROCEDURAL_FIRST) {
                    render_milk_field(buffer, audio, dt, current_effect);
                } else {
                    render_procedural(buffer, audio, dt, current_effect);
                }
            } else {
                pax_background(buffer, 0xff000000);
            }
            break;
    }
    render_algorithmic_morph(buffer);
    previous_buffer = buffer;
    previous_effect = current_effect;
}
