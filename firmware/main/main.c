// Billy fish firmware — entry point. Wires the three layers and hands off to the app runloop.
// This is the custom-firmware / direct-HTTP path; the orchestrator seam is the transport
// layer (net.*), kept isolated so an ESPHome/HA swap wouldn't touch app or hal.
#include "hal.h"
#include "net.h"
#include "runloop.h"
#include "wakeword.h"
#include "esp_log.h"

static const char *TAG = "billy";

void app_main(void)
{
    // Status LED first, standalone, so FISH_STATUS_BOOT shows immediately at power-on — before
    // the wake-word model load or WiFi join, which can each take a noticeable moment.
    fish_hal_status_init();
    fish_hal_set_status(FISH_STATUS_BOOT);

    ESP_LOGI(TAG, "Billy fish booting — ESP-IDF");

    // initialize wakeword subsystem
    if (!wakeword_init())
    {
        // Non-fatal: button mode is a complete fallback with no wake-word dependency.
        ESP_LOGW(TAG, "wake-word model failed to load — WAKEWORD mode won't detect; BUTTON mode still works");
    }

    // initialize fish hardware
    fish_hal_init();

    // initialize network
    if (net_init() != ESP_OK)   // the E2E loop needs the backend; without WiFi there's nothing to do
    {
        fish_hal_set_status(FISH_STATUS_ERROR);
        ESP_LOGE(TAG, "WiFi join failed — cannot reach the backend. Halting.");
        return;
    }

    // application
    fish_hal_set_status(FISH_STATUS_WIFI_WAIT);
    net_wait_for_backend();     // block (with backoff) until the backend is reachable
    runloop_start();            // spawns the turn loop on its own task; returns
}
