#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the WiFi radio driver only (no network association yet). Safe
// to call once at startup - this alone draws negligible power compared to
// an active connection.
esp_err_t wifi_setup_init(void);

// Connects to whatever network is already configured elsewhere on the
// device (there is no WiFi setup UI in MusicPlayer itself) and blocks (from
// the caller's task) up to timeout_ms for the connection to complete.
bool wifi_setup_connect_blocking(uint32_t timeout_ms);

// Disconnects (but leaves the radio driver initialized so a future
// wifi_setup_connect_blocking() is cheap).
void wifi_setup_disconnect(void);

bool wifi_setup_is_connected(void);

#ifdef __cplusplus
}
#endif
