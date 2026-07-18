#include "wifi_setup.h"

#include "bsp/power.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static const char *TAG = "wifi_setup";

static bool s_radio_ok = false;
static SemaphoreHandle_t s_mutex = NULL;
static int s_ref_count = 0;

esp_err_t wifi_setup_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi radio not responding, WiFi not available");
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        return ESP_OK;  // not fatal - MusicPlayer works fine without WiFi
    }
    esp_err_t stack_result = wifi_connection_init_stack();
    if (stack_result != ESP_OK) {
        ESP_LOGW(TAG, "WiFi stack initialization failed: %s; WiFi not available", esp_err_to_name(stack_result));
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        return ESP_OK;  // not fatal - MusicPlayer works fine without WiFi
    }
    s_radio_ok = true;
    return ESP_OK;
}

// Multiple independent subsystems (audio streaming, Jellyfin, Last.fm) can
// want the connection at overlapping times. wifi_connection_connect()
// unconditionally tears down and restarts the radio, and disconnect() stops
// it outright, so without reference counting one caller finishing its
// request would yank the connection out from under another caller still
// mid-stream/mid-request. Only the first caller actually connects; only the
// last caller to release actually disconnects.
bool wifi_setup_connect_blocking(uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!s_radio_ok) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ref_count > 0) {
        s_ref_count++;
        xSemaphoreGive(s_mutex);
        return true;
    }

    esp_err_t res = wifi_connect_try_all();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s (no saved networks reachable)", esp_err_to_name(res));
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_ref_count++;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "WiFi connected");
    return true;
}

void wifi_setup_disconnect(void) {
    if (!s_radio_ok) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ref_count > 0) s_ref_count--;
    bool should_disconnect = s_ref_count == 0;
    xSemaphoreGive(s_mutex);

    if (should_disconnect) {
        wifi_connection_disconnect();
        ESP_LOGI(TAG, "WiFi disconnected");
    }
}

bool wifi_setup_is_connected(void) {
    return s_radio_ok && wifi_connection_is_connected();
}
