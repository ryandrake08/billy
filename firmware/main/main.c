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
    // initialize all hardware
    fish_hal_init();

    // initialize wakeword subsystem
    if (!wakeword_init())
    {
        // Non-fatal: button mode is a complete fallback with no wake-word dependency.
        ESP_LOGW(TAG, "wake-word model failed to load — WAKEWORD mode won't detect; BUTTON mode still works");
    }

    // initialize network
    if (net_init() != ESP_OK)   // only fails on a config error (missing creds) -- not fixable by retrying
    {
        fish_hal_set_status(FISH_STATUS_ERROR);
        ESP_LOGE(TAG, "WiFi config invalid — cannot bring up networking. Halting.");
        return;
    }

    // application
    runloop_start();            // spawns the turn loop on its own task; returns
}
