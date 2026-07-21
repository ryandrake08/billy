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
    ESP_LOGI(TAG, "Billy fish booting — ESP-IDF scaffold, stubbed I/O");
    hal_init();
    net_init();
    runloop_run();   // never returns
}
