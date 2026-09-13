#include "net.h"
#include "fish_config.h"
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include <stdbool.h>

static const char *TAG = "net";

// Credentials and the backend host are injected as compile definitions from the build
// environment (main/CMakeLists.txt). The empty fallbacks for the WiFi creds only keep
// editors/clangd parsing when they don't see the build's -D flags -- wifi_connect() already
// rejects an empty WIFI_SSID at runtime. BACKEND_HOST has no such fallback: an empty value would
// silently build a nonsense URL instead of failing loudly, so a missing definition is a hard
// compile error here (CMakeLists.txt also catches it earlier, at configure time).
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef BACKEND_HOST
#error "BACKEND_HOST is not defined -- export BACKEND_HOST and run idf.py reconfigure build"
#endif

#define NET_HOSTNAME   "billy"   // DHCP hostname (option 12) so the router/dnsmasq registers us

#define WIFI_RECONNECT_BACKOFF_INITIAL_MS 1000
#define WIFI_RECONNECT_BACKOFF_MAX_MS     30000

// STT/respond/TTS timeouts live in fish_config_t -- they track backend model speed rather than
// anything fixed on the fish, so they're fetched from the shim alongside the other runtime
// tunables instead of staying compile-time here.

// GET /v1/config itself -- can't be config-driven. Kept short so a hung shim can't stall for long.
#define CONFIG_FETCH_TIMEOUT_MS 3000

// Bounds for net_fetch_config(wait_for_backend=true). Worst case (WiFi comes up but the shim
// never responds) is roughly
// FORCE_FETCH_MAX_ATTEMPTS * (CONFIG_FETCH_TIMEOUT_MS + FORCE_FETCH_RETRY_DELAY_MS).
#define FORCE_FETCH_MAX_ATTEMPTS   6
#define FORCE_FETCH_RETRY_DELAY_MS 500

// Used if a sentence event omits "voice" (older shim, or the shim just didn't set one) -- keeps
// TTS working rather than sending Kokoro an empty voice field.
#define DEFAULT_TTS_VOICE "am_onyx"

// --- WiFi station bring-up ---------------------------------------------------------------

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif;     // the STA netif, kept so we can set its DHCP hostname
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_DISCONNECTED_BIT BIT1   // pulsed to wake wifi_monitor_task; auto-clears on read

// Backoff for the *next* reconnect attempt. Only wifi_monitor_task advances it (on a failed
// attempt); the event handler resets it to the floor on GOT_IP. A relaxed read/write race between
// the two is fine — worst case is one extra short-delay retry, not a correctness issue.
static uint32_t s_reconnect_backoff_ms;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        // The netif is started now, before association/DHCP — set the hostname so it rides along
        // in the initial DHCP request. Non-fatal if it doesn't take.
        esp_err_t herr = esp_netif_set_hostname(s_netif, NET_HOSTNAME);
        if (herr != ESP_OK) ESP_LOGW(TAG, "set hostname failed: %s", esp_err_to_name(herr));
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        // L2 association only -- DHCP hasn't necessarily finished yet, so this isn't "usable" on
        // its own (see IP_EVENT_STA_GOT_IP below). Logged purely to separate slow-auth from
        // slow-DHCP when debugging a flaky AP.
        ESP_LOGI(TAG, "WiFi associated — waiting on DHCP");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // Never give up: this fires for every drop for the life of the app, not just at boot, so
        // the fish always finds its way back onto the network on its own.
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_reconnect_backoff_ms = WIFI_RECONNECT_BACKOFF_INITIAL_MS;
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
    {
        // Still associated at L2, but the DHCP lease is gone (e.g. the router's DHCP server
        // bounced without dropping our association) -- no WIFI_EVENT_STA_DISCONNECTED fires for
        // this on its own, so without this branch net_is_connected() would keep lying. Force a
        // real disconnect rather than calling esp_wifi_connect() straight from an
        // already-associated state (unreliable for forcing a fresh DHCP request) -- that in turn
        // fires WIFI_EVENT_STA_DISCONNECTED, reusing the same mark-down + backoff-retry path
        // already proven for a real link drop.
        ESP_LOGW(TAG, "lost DHCP lease — forcing reconnect");
        esp_wifi_disconnect();
    }
}

// Supervises the WiFi link for the app's entire runtime (not just at boot): sleeps on the
// disconnected-event bit, then retries with capped exponential backoff, forever. This is what
// makes recovery unbounded -- there's always a task watching, so a dropped link (router reboot,
// AP roam, temporary outage) is retried indefinitely instead of being given up on after a fixed
// count.
static void wifi_monitor_task(void *arg)
{
    (void) arg;
    for (;;)
    {
        xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        uint32_t backoff_ms = s_reconnect_backoff_ms;
        ESP_LOGW(TAG, "WiFi disconnected — retrying in %lu ms", (unsigned long) backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        // A reconnect may have already landed while we were waiting out the backoff (e.g. this
        // was a stale event from before a fast auto-reassociation) -- don't step on it.
        if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) continue;
        s_reconnect_backoff_ms = (backoff_ms * 2 > WIFI_RECONNECT_BACKOFF_MAX_MS)
            ? WIFI_RECONNECT_BACKOFF_MAX_MS : backoff_ms * 2;
        esp_wifi_connect();
    }
}

// True once an IP is held. Lets callers that are about to make a backend HTTP call skip it
// immediately when the link is known down, rather than waiting out a connect timeout to learn
// the same thing.
static bool net_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

// Bring up the WiFi driver and kick off the first join attempt, then return -- doesn't wait for
// an IP. wifi_event_handler + wifi_monitor_task carry the connection the rest of the way,
// asynchronously, for as long as the app runs.
static esp_err_t wifi_start(void)
{
    if (WIFI_SSID[0] == '\0')
    {
        ESP_LOGE(TAG, "WIFI_SSID is empty — export WIFI_SSID/WIFI_PASSWORD and rebuild "
                      "(idf.py reconfigure build)");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_events = xEventGroupCreate();
    s_reconnect_backoff_ms = WIFI_RECONNECT_BACKOFF_INITIAL_MS;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // The driver's own internal connection-state tracing (tag "wifi") is chatty at INFO. The
    // asynchronous join can interleave it mid-line with our own logging on the shared UART.
    // Quiet it to warnings-and-up; this doesn't touch any other tag's level.
    esp_log_level_set("wifi", ESP_LOG_WARN);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL));

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

    xTaskCreate(wifi_monitor_task, "wifi_monitor", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "joining WiFi \"%s\" (async)...", WIFI_SSID);
    return ESP_OK;
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

    return wifi_start();
}

// --- Transport contract ------------------------------------------------------------------
// The three calls src/client/billy_cli.py demonstrates: STT direct to whisper /inference (:8081),
// the brain hop to the shim /v1/respond (:8000, SSE), TTS direct to Kokoro /v1/audio/speech
// (:8880).

// Extract a top-level JSON string field — {"key":"value with \"escapes\""} — into `out`.
// Both sources we parse emit raw UTF-8 (the shim with ensure_ascii=False, whisper.cpp natively),
// so non-ASCII passes straight through and there's no \uXXXX to decode. Avoids a cJSON dependency.
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[48];
    int patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len)
    {
        char c = *p;
        if (c == '\\' && p[1])
        {
            p++;
            switch (*p)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = *p;   break;   // ", \, / pass through
            }
        }
        out[o++] = c;
        p++;
    }
    out[o] = '\0';
    return true;
}

static void wr_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Build a 44-byte canonical PCM WAV header for `nsamples` mono 16-bit samples at `rate` Hz.
static void wav_header(uint8_t *h, uint32_t nsamples, uint32_t rate)
{
    uint32_t data_bytes = nsamples * 2;
    memcpy(h, "RIFF", 4);          wr_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);     wr_le32(h + 16, 16);
    wr_le16(h + 20, 1);            wr_le16(h + 22, 1);            // PCM, mono
    wr_le32(h + 24, rate);         wr_le32(h + 28, rate * 2);     // sample rate, byte rate
    wr_le16(h + 32, 2);            wr_le16(h + 34, 16);           // block align, bits
    memcpy(h + 36, "data", 4);     wr_le32(h + 40, data_bytes);
}

static uint16_t rd_le16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Escape a C string for embedding inside a JSON string literal.
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    while (*in && o + 2 < out_len)
    {
        char c = *in++;
        switch (c)
        {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:   if ((unsigned char) c >= 0x20) out[o++] = c; break;   // drop other controls
        }
    }
    out[o] = '\0';
}

// STT: POST the utterance as a WAV to whisper /inference (multipart, streamed so we don't buffer
// the whole multipart body), parse the {"text": ...} JSON reply. STT is a dumb audio->text
// transform, so it's called directly (only the brain hop goes through the shim).
esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len,
                   char *out_language, size_t out_language_len)
{
    out_text[0] = '\0';
    snprintf(out_language, out_language_len, "english");
    if (!audio || !audio->samples || audio->count == 0)
    {
        ESP_LOGW(TAG, "STT: nothing captured");
        return ESP_OK;
    }
    if (!net_is_connected())
    {
        ESP_LOGW(TAG, "STT: no WiFi — skipping");
        return ESP_FAIL;
    }

    static const char *BOUNDARY = "----billyfishboundary";
    char pre[224];
    int prelen = snprintf(pre, sizeof pre,
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BOUNDARY);
    // whisper.cpp's server defaults each request to its launch-time -l flag, and decoding non-English
    // audio as English silently produces an English *translation* instead of a transcript.
    char post[384];
    int postlen = snprintf(post, sizeof post,
        "\r\n--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nauto\r\n"
        "--%s--\r\n", BOUNDARY, BOUNDARY, BOUNDARY);
    uint8_t wav[44];
    wav_header(wav, audio->count, audio->sample_rate);
    size_t pcm_bytes = audio->count * sizeof(int16_t);
    int total = prelen + (int) sizeof(wav) + (int) pcm_bytes + postlen;

    char url[128];
    snprintf(url, sizeof url, "http://%s:8081/inference", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
                                      .timeout_ms = fish_config_get()->stt_timeout_ms };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "STT: esp_http_client_init failed (heap/PSRAM exhausted?)");
        return ESP_ERR_NO_MEM;
    }
    char ctype[80];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", ctype);

    esp_err_t err = esp_http_client_open(client, total);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "STT connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_write(client, pre, prelen);
    esp_http_client_write(client, (const char *) wav, sizeof wav);
    const char *p = (const char *) audio->samples;
    size_t left = pcm_bytes;
    while (left > 0)
    {
        int chunk = left > 4096 ? 4096 : (int) left;
        int w = esp_http_client_write(client, p, chunk);
        if (w < 0) { err = ESP_FAIL; break; }
        p += w;
        left -= w;
    }
    esp_http_client_write(client, post, postlen);

    // verbose_json's per-word timestamp/probability entries make this reply far larger than
    // plain json's bare {"text": ...} (measured ~470 bytes/sec of speech against gpu-host) --
    // size for the firmware's capture_max_ms cap (10 s default) with real margin, and take it
    // from PSRAM rather than the runloop task's 12 KB stack.
    size_t resp_cap = 8192;
    char *resp = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM);
    if (!resp)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    int rd = 0;
    if (err == ESP_OK)
    {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        rd = esp_http_client_read_response(client, resp, resp_cap - 1);
        if (rd < 0) rd = 0;
        resp[rd] = '\0';
        if (status != 200)
        {
            ESP_LOGE(TAG, "STT HTTP %d: %s", status, resp);
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) { heap_caps_free(resp); return err; }

    if (json_get_string(resp, "text", out_text, out_len))
    {
        char *s = out_text;
        while (*s == ' ') s++;   // whisper prefixes a leading space
        if (s != out_text) memmove(out_text, s, strlen(s) + 1);
        size_t len = strlen(out_text);   // and appends a trailing newline
        while (len > 0 && (unsigned char) out_text[len - 1] <= ' ') out_text[--len] = '\0';
    }
    json_get_string(resp, "detected_language", out_language, out_language_len);
    heap_caps_free(resp);
    ESP_LOGI(TAG, "STT -> \"%s\" [%s]", out_text, out_language);
    return ESP_OK;
}

// Brain: POST {session, text} to the shim and stream the reply. The shim owns persona/history/
// cleaning and emits SSE lines `data: {"sentence": "..."}` (spoken-ready) then `data: [DONE]`;
// on_sentence fires for each so TTS/playback pipelines with generation.
esp_err_t net_respond(const char *text, const char *language, sentence_cb_t on_sentence, void *ctx)
{
    if (!net_is_connected())
    {
        ESP_LOGW(TAG, "shim: no WiFi — skipping");
        return ESP_FAIL;
    }

    char esc[256 * 2];
    json_escape(text, esc, sizeof esc);
    char esc_language[128];
    json_escape(language, esc_language, sizeof esc_language);
    char body[600 + sizeof esc_language];
    int bodylen = snprintf(body, sizeof body,
        "{\"session\":\"default\",\"text\":\"%s\",\"language\":\"%s\"}", esc, esc_language);

    char url[128];
    snprintf(url, sizeof url, "http://%s:8000/v1/respond", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
                                      .timeout_ms = fish_config_get()->respond_timeout_ms };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "shim: esp_http_client_init failed (heap/PSRAM exhausted?)");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    esp_err_t err = esp_http_client_open(client, bodylen);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "shim connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_write(client, body, bodylen);
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
        ESP_LOGE(TAG, "shim HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Reassemble the byte stream into lines, then parse SSE `data:` events.
    char line[512];
    int linelen = 0;
    char chunk[256];
    bool done = false;
    esp_err_t cb_err = ESP_OK;
    while (!done)
    {
        int r = esp_http_client_read(client, chunk, sizeof chunk);
        if (r <= 0) break;   // stream closed
        for (int i = 0; i < r && !done; i++)
        {
            char ch = chunk[i];
            if (ch == '\n')
            {
                line[linelen] = '\0';
                linelen = 0;
                if (strncmp(line, "data:", 5) != 0) continue;
                char *pl = line + 5;
                while (*pl == ' ') pl++;
                if (strcmp(pl, "[DONE]") == 0)
                {
                    done = true;
                }
                else
                {
                    char sentence[256];
                    if (json_get_string(pl, "sentence", sentence, sizeof sentence))
                    {
                        char voice[32];
                        if (!json_get_string(pl, "voice", voice, sizeof voice))
                        {
                            strlcpy(voice, DEFAULT_TTS_VOICE, sizeof voice);
                        }
                        ESP_LOGI(TAG, "shim -> \"%s\" (voice=%s)", sentence, voice);
                        cb_err = on_sentence(sentence, voice, ctx);
                        // A fatal local failure (audio hardware or motor shutdown, not the stream
                        // itself) -- stop rather than fetching/discarding the rest of the reply.
                        if (cb_err != ESP_OK) done = true;
                    }
                    else if (json_get_string(pl, "error", sentence, sizeof sentence))
                    {
                        ESP_LOGE(TAG, "shim error: %s", sentence);
                    }
                }
            }
            else if (ch != '\r' && linelen < (int) sizeof(line) - 1)
            {
                line[linelen++] = ch;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return cb_err;
}

// TTS: POST one sentence to Kokoro, in the given voice, and decode the returned WAV into mono
// 16-bit PCM (PSRAM).
esp_err_t net_tts(const char *sentence, const char *voice, audio_buf_t *out_audio)
{
    out_audio->samples = NULL;
    out_audio->count = 0;
    out_audio->sample_rate = 0;

    if (!net_is_connected())
    {
        ESP_LOGW(TAG, "TTS: no WiFi — skipping");
        return ESP_FAIL;
    }

    char esc[256 * 2];
    json_escape(sentence, esc, sizeof esc);
    char body[600];
    int bodylen = snprintf(body, sizeof body,
        "{\"model\":\"kokoro\",\"input\":\"%s\",\"voice\":\"%s\",\"response_format\":\"wav\"}", esc, voice);

    char url[128];
    snprintf(url, sizeof url, "http://%s:8880/v1/audio/speech", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
                                      .timeout_ms = fish_config_get()->tts_timeout_ms };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "TTS: esp_http_client_init failed (heap/PSRAM exhausted?)");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, bodylen);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TTS connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_write(client, body, bodylen);
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
        ESP_LOGE(TAG, "TTS HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const size_t MAX_WAV = 640 * 1024;   // ~13 s @ 24 kHz mono 16-bit
    size_t cap = (content_len > 0 && (size_t) content_len <= MAX_WAV) ? (size_t) content_len : MAX_WAV;
    uint8_t *wav = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!wav)
    {
        ESP_LOGE(TAG, "TTS: PSRAM alloc %u failed", (unsigned) cap);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    while (total < cap)
    {
        int r = esp_http_client_read(client, (char *) wav + total, cap - total);
        if (r <= 0) break;
        total += r;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Walk the WAV chunks from offset 12 for "fmt " (rate/channels/bits) and "data".
    if (total < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
    {
        ESP_LOGE(TAG, "TTS: not a WAV (%u bytes)", (unsigned) total);
        heap_caps_free(wav);
        return ESP_FAIL;
    }
    uint32_t rate = 24000;
    uint16_t channels = 1, bits = 16;
    size_t data_off = 0, data_size = 0, off = 12;
    while (off + 8 <= total)
    {
        uint32_t csize = rd_le32(wav + off + 4);
        if (memcmp(wav + off, "fmt ", 4) == 0 && off + 8 + 16 <= total)
        {
            channels = rd_le16(wav + off + 8 + 2);
            rate     = rd_le32(wav + off + 8 + 4);
            bits     = rd_le16(wav + off + 8 + 14);
        }
        else if (memcmp(wav + off, "data", 4) == 0)
        {
            data_off = off + 8;
            data_size = csize;
            break;
        }
        off += 8 + csize + (csize & 1);   // chunks are word-aligned
    }
    if (data_off == 0 || bits != 16 || (channels != 1 && channels != 2))
    {
        ESP_LOGE(TAG, "TTS: unsupported WAV (ch=%u bits=%u)", channels, bits);
        heap_caps_free(wav);
        return ESP_FAIL;
    }
    if (data_size > total - data_off) data_size = total - data_off;   // clamp to bytes actually read

    const int16_t *src = (const int16_t *) (wav + data_off);
    size_t nframes = data_size / (2 * channels);
    int16_t *pcm = heap_caps_malloc(nframes * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm)
    {
        heap_caps_free(wav);
        return ESP_ERR_NO_MEM;
    }
    if (channels == 1)
    {
        memcpy(pcm, src, nframes * sizeof(int16_t));
    }
    else   // downmix stereo -> mono
    {
        for (size_t i = 0; i < nframes; i++)
            pcm[i] = (int16_t) (((int) src[2 * i] + src[2 * i + 1]) / 2);
    }
    heap_caps_free(wav);

    out_audio->samples = pcm;
    out_audio->count = nframes;
    out_audio->sample_rate = (int) rate;
    ESP_LOGI(TAG, "TTS -> %u samples @ %u Hz", (unsigned) nframes, (unsigned) rate);
    return ESP_OK;
}

// --- Runtime config -------------------------------------------------------------------------
// Bench-measured tunables that are likely to need retuning once the fish is in its final
// housing. Nothing caches a value from fish_config_get() -- every read is fresh -- so a value
// applied here takes effect on the very next read, and a shim-side edit reaches a WAKEWORD-mode
// fish (which never reboots) with no reboot at all.

// Single best-effort attempt: on any failure (no WiFi, shim unreachable, bad response) the
// compiled-in or previously-fetched values already in effect are left untouched -- there's
// always something sane to run on.
static esp_err_t net_fetch_config_once(void)
{
    if (!net_is_connected())
    {
        ESP_LOGW(TAG, "config: no WiFi — using compiled-in defaults");
        return ESP_FAIL;
    }

    char url[128];
    snprintf(url, sizeof url, "http://%s:8000/v1/config", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_GET,
                                      .timeout_ms = CONFIG_FETCH_TIMEOUT_MS };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGW(TAG, "config: esp_http_client_init failed — using compiled-in defaults");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "config: connect failed (%s) — using compiled-in defaults", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char resp[1024];
    int rd = esp_http_client_read_response(client, resp, sizeof resp - 1);
    if (rd < 0) rd = 0;
    resp[rd] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200)
    {
        ESP_LOGW(TAG, "config: HTTP %d — using compiled-in defaults", status);
        return ESP_FAIL;
    }

    fish_config_apply_json(resp);
    ESP_LOGI(TAG, "config: applied from shim");
    return ESP_OK;
}

esp_err_t net_fetch_config(bool wait_for_backend)
{
    if (!wait_for_backend)
    {
        return net_fetch_config_once();
    }

    // wait_for_backend=true: retries both "WiFi not up yet" and "shim not reachable yet" alike --
    // net_fetch_config_once() already fails fast in the first case and within
    // CONFIG_FETCH_TIMEOUT_MS in the second, so simply retrying it covers both. Still bounded, so
    // a genuinely unreachable network degrades exactly as it always has (STT's own "no WiFi" skip
    // + the runloop's error tone) instead of leaving the wake cycle stuck silent forever.
    for (int attempt = 1; attempt <= FORCE_FETCH_MAX_ATTEMPTS; attempt++)
    {
        if (net_fetch_config_once() == ESP_OK) return ESP_OK;
        if (attempt < FORCE_FETCH_MAX_ATTEMPTS)
        {
            vTaskDelay(pdMS_TO_TICKS(FORCE_FETCH_RETRY_DELAY_MS));
        }
    }
    ESP_LOGW(TAG, "config: still unavailable after %d attempts — proceeding on current config",
             FORCE_FETCH_MAX_ATTEMPTS);
    return ESP_FAIL;
}
