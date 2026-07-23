// Billy fish firmware — entry point. Wires the three layers (SCOPING.md §8.1) and hands off
// to the app runloop. This is the custom-firmware / direct-HTTP path; the orchestrator seam
// is the transport layer (net.*), kept isolated so an ESPHome/HA swap wouldn't touch app or hal.
#include "hal.h"
#include "net.h"
#include "runloop.h"
#include "esp_log.h"

static const char *TAG = "billy";

void app_main(void)
{
    ESP_LOGI(TAG, "Billy fish booting — ESP-IDF, Stage 3.1 audio bring-up");
    fish_hal_init();

    // Bench bring-up: verify the mic + amp wiring first. This loops (a test tone, then a live
    // mic-level readout); remove the call to resume normal boot — WiFi + runloop — below.
    fish_hal_selftest();

    if (net_init() == ESP_OK)   // join WiFi
    {
        net_health_check();     // "hello backend" — prove the network path (Stage 2.2)
    }
    runloop_run();   // never returns
}
