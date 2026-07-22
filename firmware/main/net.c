#include "net.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

static const char *TAG = "net";

// Credentials and the backend host are injected as compile definitions from the build
// environment (main/CMakeLists.txt); BACKEND_HOST is required there. The empty fallbacks for the
// WiFi creds only keep editors/clangd parsing when they don't see the build's -D flags.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#define WIFI_MAX_RETRY 8

// --- WiFi station bring-up ---------------------------------------------------------------

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retries;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retries < WIFI_MAX_RETRY)
        {
            s_retries++;
            ESP_LOGW(TAG, "WiFi disconnected — retry %d/%d", s_retries, WIFI_MAX_RETRY);
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void)
{
    if (WIFI_SSID[0] == '\0')
    {
        ESP_LOGE(TAG, "WIFI_SSID is empty — export WIFI_SSID/WIFI_PASSWORD and rebuild "
                      "(idf.py reconfigure build)");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *) wifi_cfg.sta.ssid, WIFI_SSID, sizeof wifi_cfg.sta.ssid);
    strlcpy((char *) wifi_cfg.sta.password, WIFI_PASSWORD, sizeof wifi_cfg.sta.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "joining WiFi \"%s\"...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to join WiFi \"%s\" after %d retries", WIFI_SSID, WIFI_MAX_RETRY);
    return ESP_FAIL;
}

esp_err_t net_init(void)
{
    // NVS is required by the WiFi driver (it caches calibration/config there).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    return wifi_connect();
}

// "Hello backend" reachability proof (Stage 2.2): a single GET to the shim's /health. The
// default HTTP event handler discards the body — we only care that the round-trip succeeds.
esp_err_t net_health_check(void)
{
    const char *url = "http://" BACKEND_HOST ":8000/health";
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "hello backend — GET %s -> HTTP %d", url, status);
        if (status != 200)
        {
            err = ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "hello backend FAILED — GET %s: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

// --- Transport contract (still stubbed until Stage 3) ------------------------------------
// These log and return canned values so the runloop exercises the full turn shape. The real
// impl uses esp_http_client against whisper /inference (:8081), the shim /v1/respond (:8000,
// SSE), and Kokoro /v1/audio/speech (:8880) — the contract client/billy_cli.py demonstrates.

esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len)
{
    (void) audio;
    ESP_LOGI(TAG, "STT -> whisper /inference (stub)");
    strlcpy(out_text, "what time is it", out_len);
    return ESP_OK;
}

esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx)
{
    ESP_LOGI(TAG, "brain -> shim /v1/respond, text=\"%s\" (stub)", text);
    // Two canned sentences to exercise pipelined per-sentence playback.
    on_sentence("Well now, I'm just a fish on a wall.", ctx);
    on_sentence("I can't see a clock from up here, partner.", ctx);
    return ESP_OK;
}

esp_err_t net_tts(const char *sentence, audio_buf_t *out_audio)
{
    ESP_LOGI(TAG, "TTS -> Kokoro /v1/audio/speech: \"%s\" (stub)", sentence);
    out_audio->len = strlen(sentence);
    return ESP_OK;
}
