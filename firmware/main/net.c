#include "net.h"
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

#define WIFI_MAX_RETRY 8
#define NET_HOSTNAME   "billy"   // DHCP hostname (option 12) so the router/dnsmasq registers us

#define BACKEND_BACKOFF_INITIAL_MS 1000
#define BACKEND_BACKOFF_MAX_MS     30000

#define HEALTH_CHECK_TIMEOUT_MS 5000
#define STT_TIMEOUT_MS          30000    // whisper.cpp transcribing the whole utterance
#define RESPOND_TIMEOUT_MS      120000   // shim SSE stream: LLM generation across the whole reply
#define TTS_TIMEOUT_MS          120000   // Kokoro synthesizing one sentence

// --- WiFi station bring-up ---------------------------------------------------------------

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif;     // the STA netif, kept so we can set its DHCP hostname
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retries;

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
    s_netif = esp_netif_create_default_wifi_sta();

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

// "Hello backend" reachability probe: a single GET to the shim's /health. The default HTTP event
// handler discards the body — we only care that the round-trip succeeds.
static esp_err_t net_health_check(void)
{
    const char *url = "http://" BACKEND_HOST ":8000/health";
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HEALTH_CHECK_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "hello backend: esp_http_client_init failed (heap/PSRAM exhausted?)");
        return ESP_ERR_NO_MEM;
    }

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

// Block until the backend answers, retrying with capped exponential backoff. The fish is useless
// without the backend, so we wait here rather than taking turns that would all fail — and this
// lets it recover on its own if the backend is slow to come up (e.g. a reboot).
void net_wait_for_backend(void)
{
    int backoff_ms = BACKEND_BACKOFF_INITIAL_MS;
    while (net_health_check() != ESP_OK)
    {
        ESP_LOGW(TAG, "backend unreachable — retrying in %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms * 2 > BACKEND_BACKOFF_MAX_MS) ? BACKEND_BACKOFF_MAX_MS : backoff_ms * 2;
    }
}

// --- Transport contract ------------------------------------------------------------------
// The three calls client/billy_cli.py demonstrates: STT direct to whisper /inference (:8081),
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
esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len)
{
    out_text[0] = '\0';
    if (!audio || !audio->samples || audio->count == 0)
    {
        ESP_LOGW(TAG, "STT: nothing captured");
        return ESP_OK;
    }

    static const char *BOUNDARY = "----billyfishboundary";
    char pre[224];
    int prelen = snprintf(pre, sizeof pre,
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BOUNDARY);
    char post[288];
    int postlen = snprintf(post, sizeof post,
        "\r\n--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
        "--%s--\r\n", BOUNDARY, BOUNDARY);
    uint8_t wav[44];
    wav_header(wav, audio->count, audio->sample_rate);
    size_t pcm_bytes = audio->count * sizeof(int16_t);
    int total = prelen + (int) sizeof(wav) + (int) pcm_bytes + postlen;

    char url[128];
    snprintf(url, sizeof url, "http://%s:8081/inference", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = STT_TIMEOUT_MS };
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

    char resp[2048];
    int rd = 0;
    if (err == ESP_OK)
    {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        rd = esp_http_client_read_response(client, resp, sizeof resp - 1);
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
    if (err != ESP_OK) return err;

    if (json_get_string(resp, "text", out_text, out_len))
    {
        char *s = out_text;
        while (*s == ' ') s++;   // whisper prefixes a leading space
        if (s != out_text) memmove(out_text, s, strlen(s) + 1);
        size_t len = strlen(out_text);   // and appends a trailing newline
        while (len > 0 && (unsigned char) out_text[len - 1] <= ' ') out_text[--len] = '\0';
    }
    ESP_LOGI(TAG, "STT -> \"%s\"", out_text);
    return ESP_OK;
}

// Brain: POST {session, text} to the shim and stream the reply. The shim owns persona/history/
// cleaning and emits SSE lines `data: {"sentence": "..."}` (spoken-ready) then `data: [DONE]`;
// on_sentence fires for each so TTS/playback pipelines with generation.
esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx)
{
    char esc[256 * 2];
    json_escape(text, esc, sizeof esc);
    char body[600];
    int bodylen = snprintf(body, sizeof body, "{\"session\":\"default\",\"text\":\"%s\"}", esc);

    char url[128];
    snprintf(url, sizeof url, "http://%s:8000/v1/respond", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = RESPOND_TIMEOUT_MS };
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
                        ESP_LOGI(TAG, "shim -> \"%s\"", sentence);
                        cb_err = on_sentence(sentence, ctx);
                        // A fatal local failure (audio hardware, not the stream itself) -- stop
                        // reading rather than fetching/discarding the rest of the reply for nothing.
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

// TTS: POST one sentence to Kokoro and decode the returned WAV into mono 16-bit PCM (PSRAM).
esp_err_t net_tts(const char *sentence, audio_buf_t *out_audio)
{
    out_audio->samples = NULL;
    out_audio->count = 0;
    out_audio->sample_rate = 0;

    char esc[256 * 2];
    json_escape(sentence, esc, sizeof esc);
    char body[600];
    int bodylen = snprintf(body, sizeof body,
        "{\"model\":\"kokoro\",\"input\":\"%s\",\"voice\":\"am_onyx\",\"response_format\":\"wav\"}", esc);

    char url[128];
    snprintf(url, sizeof url, "http://%s:8880/v1/audio/speech", BACKEND_HOST);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = TTS_TIMEOUT_MS };
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
