#include "audio_analysis.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define FFT_SIZE 1024
#define FFT_HOP  512

static const char *TAG = "analysis";

typedef struct {
    int16_t samples[FFT_SIZE];
    uint32_t sample_rate;
} analysis_block_t;

static QueueHandle_t analysis_queue;
static int16_t       capture_ring[FFT_SIZE];
static size_t        capture_pos;
static size_t        capture_total;
static size_t        samples_since_block;
static portMUX_TYPE  capture_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE  snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_analysis_snapshot_t snapshot;

static float fft_data[FFT_SIZE * 2] __attribute__((aligned(16)));
static float fft_window[FFT_SIZE] __attribute__((aligned(16)));

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void analyze_block(const analysis_block_t *block) {
    static const float edges[AUDIO_ANALYSIS_BANDS + 1] = {
        35, 70, 140, 280, 560, 1100, 2200, 3600, 5600, 8000, 11000, 16000, 22050,
    };
    static float previous[AUDIO_ANALYSIS_BANDS];
    static float smoothed[AUDIO_ANALYSIS_BANDS];
    static float flux_average = 0.015f;
    static float rms_smooth;
    static float peak_smooth;
    static int64_t last_beat_us;

    double sum_sq = 0.0;
    float  peak    = 0.0f;
    for (size_t i = 0; i < FFT_SIZE; i++) {
        float sample = block->samples[i] / 32768.0f;
        float abs_sample = fabsf(sample);
        if (abs_sample > peak) peak = abs_sample;
        sum_sq += sample * sample;
        fft_data[i * 2]     = sample * fft_window[i];
        fft_data[i * 2 + 1] = 0.0f;
    }

    float rms = sqrtf((float)(sum_sq / FFT_SIZE));
    rms_smooth  = rms > rms_smooth ? rms_smooth * 0.35f + rms * 0.65f : rms_smooth * 0.88f + rms * 0.12f;
    peak_smooth = peak > peak_smooth ? peak : peak_smooth * 0.92f;

    dsps_fft2r_fc32(fft_data, FFT_SIZE);
    dsps_bit_rev_fc32(fft_data, FFT_SIZE);

    const float sample_rate = block->sample_rate ? block->sample_rate : 44100.0f;
    float gain = 0.16f / fmaxf(rms_smooth, 0.012f);
    if (gain < 0.8f) gain = 0.8f;
    if (gain > 10.0f) gain = 10.0f;

    float flux = 0.0f;
    for (size_t band = 0; band < AUDIO_ANALYSIS_BANDS; band++) {
        int first = (int)ceilf(edges[band] * FFT_SIZE / sample_rate);
        int last  = (int)floorf(edges[band + 1] * FFT_SIZE / sample_rate);
        if (first < 1) first = 1;
        if (last <= first) last = first + 1;
        if (last >= FFT_SIZE / 2) last = FFT_SIZE / 2 - 1;

        float maximum = 0.0f;
        for (int bin = first; bin <= last; bin++) {
            float re  = fft_data[bin * 2];
            float im  = fft_data[bin * 2 + 1];
            float mag = sqrtf(re * re + im * im) * (2.0f / FFT_SIZE);
            if (mag > maximum) maximum = mag;
        }
        float value = clamp01(maximum * gain * 3.5f);
        smoothed[band] = value > smoothed[band] ? smoothed[band] * 0.25f + value * 0.75f
                                                : smoothed[band] * 0.84f + value * 0.16f;
        if (band < 5 && smoothed[band] > previous[band]) flux += smoothed[band] - previous[band];
        previous[band] = smoothed[band];
    }

    flux_average = flux_average * 0.94f + flux * 0.06f;
    float beat_strength = clamp01((flux - flux_average * 1.25f) * 3.0f);
    int64_t now = esp_timer_get_time();
    bool beat = beat_strength > 0.18f && smoothed[2] > 0.10f && now - last_beat_us > 180000;
    if (beat) last_beat_us = now;

    portENTER_CRITICAL(&snapshot_lock);
    snapshot.rms           = clamp01(rms_smooth * gain * 2.2f);
    snapshot.peak          = clamp01(peak_smooth * gain);
    snapshot.bass          = (smoothed[0] + smoothed[1] + smoothed[2] + smoothed[3]) * 0.25f;
    snapshot.mid           = (smoothed[4] + smoothed[5] + smoothed[6] + smoothed[7]) * 0.25f;
    snapshot.treble        = (smoothed[8] + smoothed[9] + smoothed[10] + smoothed[11]) * 0.25f;
    snapshot.beat_strength = beat ? beat_strength : snapshot.beat_strength * 0.78f;
    if (beat) snapshot.beat_counter++;
    memcpy(snapshot.bands, smoothed, sizeof(smoothed));
    memcpy(snapshot.waveform, &block->samples[FFT_SIZE - AUDIO_ANALYSIS_WAVEFORM], sizeof(snapshot.waveform));
    portEXIT_CRITICAL(&snapshot_lock);
}

static void analysis_task(void *unused) {
    (void)unused;
    analysis_block_t block;
    while (true) {
        if (xQueueReceive(analysis_queue, &block, portMAX_DELAY) == pdTRUE) analyze_block(&block);
    }
}

esp_err_t audio_analysis_init(void) {
    analysis_queue = xQueueCreate(2, sizeof(analysis_block_t));
    if (analysis_queue == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) return err;
    dsps_wind_hann_f32(fft_window, FFT_SIZE);
    if (xTaskCreatePinnedToCore(analysis_task, "audio-analysis", 6144, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "1024-point audio analyzer ready");
    return ESP_OK;
}

void audio_analysis_feed_s16(const int16_t *pcm, size_t frames, uint8_t channels, uint32_t sample_rate) {
    if (pcm == NULL || channels == 0 || analysis_queue == NULL) return;

    for (size_t frame = 0; frame < frames; frame++) {
        int32_t mono = pcm[frame * channels];
        if (channels > 1) mono = (mono + pcm[frame * channels + 1]) / 2;

        bool make_block = false;
        portENTER_CRITICAL(&capture_lock);
        capture_ring[capture_pos] = (int16_t)mono;
        capture_pos = (capture_pos + 1) % FFT_SIZE;
        if (capture_total < FFT_SIZE) capture_total++;
        samples_since_block++;
        if (capture_total == FFT_SIZE && samples_since_block >= FFT_HOP) {
            samples_since_block = 0;
            make_block = true;
        }
        portEXIT_CRITICAL(&capture_lock);

        if (make_block) {
            analysis_block_t block = {.sample_rate = sample_rate};
            portENTER_CRITICAL(&capture_lock);
            size_t start = capture_pos;
            for (size_t i = 0; i < FFT_SIZE; i++) block.samples[i] = capture_ring[(start + i) % FFT_SIZE];
            portEXIT_CRITICAL(&capture_lock);
            xQueueSend(analysis_queue, &block, 0);  // Dropping analysis is preferable to stalling audio.
        }
    }
}

void audio_analysis_get(audio_analysis_snapshot_t *out) {
    if (out == NULL) return;
    portENTER_CRITICAL(&snapshot_lock);
    *out = snapshot;
    portEXIT_CRITICAL(&snapshot_lock);
}

void audio_analysis_reset(void) {
    portENTER_CRITICAL(&capture_lock);
    memset(capture_ring, 0, sizeof(capture_ring));
    capture_pos = capture_total = samples_since_block = 0;
    portEXIT_CRITICAL(&capture_lock);
    portENTER_CRITICAL(&snapshot_lock);
    memset(&snapshot, 0, sizeof(snapshot));
    portEXIT_CRITICAL(&snapshot_lock);
    if (analysis_queue) xQueueReset(analysis_queue);
}
