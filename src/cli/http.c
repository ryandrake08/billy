#include "http.h"
#include "json_util.h"
#include "wav.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void http_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
void http_global_cleanup(void) { curl_global_cleanup(); }

// Growable response-body sink for the non-streaming calls (STT, TTS).
struct membuf
{
    uint8_t *data;
    size_t   len;
    size_t   cap;
};

static size_t membuf_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct membuf *mb = userdata;
    size_t n = size * nmemb;
    if (mb->len + n + 1 > mb->cap)
    {
        size_t newcap = mb->cap ? mb->cap * 2 : 4096;
        while (newcap < mb->len + n + 1) newcap *= 2;
        uint8_t *p = realloc(mb->data, newcap);
        if (!p) return 0;   // signals an error to curl, aborts the transfer
        mb->data = p;
        mb->cap = newcap;
    }
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    mb->data[mb->len] = '\0';   // keep it NUL-terminated for the JSON-text callers
    return n;
}

bool http_stt(const char *stt_url, const int16_t *pcm, size_t nsamples, uint32_t sample_rate,
              char *out_text, size_t out_len, long timeout_ms)
{
    out_text[0] = '\0';
    if (!pcm || nsamples == 0) return true;   // nothing captured, not an error

    size_t wav_len = WAV_HEADER_BYTES + nsamples * sizeof(int16_t);
    uint8_t *wav = malloc(wav_len);
    if (!wav) return false;
    wav_write_header(wav, (uint32_t) nsamples, sample_rate);
    memcpy(wav + WAV_HEADER_BYTES, pcm, nsamples * sizeof(int16_t));

    CURL *curl = curl_easy_init();
    if (!curl) { free(wav); return false; }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "rec.wav");
    curl_mime_type(part, "audio/wav");
    curl_mime_data(part, (const char *) wav, wav_len);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    char url[256];
    snprintf(url, sizeof url, "%s/inference", stt_url);

    struct membuf resp = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = (rc == CURLE_OK && status == 200);

    if (ok && resp.data)
    {
        if (json_get_string((const char *) resp.data, "text", out_text, out_len))
        {
            char *s = out_text;
            while (*s == ' ') s++;   // whisper prefixes a leading space
            if (s != out_text) memmove(out_text, s, strlen(s) + 1);
            size_t l = strlen(out_text);   // and appends a trailing newline
            while (l > 0 && (unsigned char) out_text[l - 1] <= ' ') out_text[--l] = '\0';
        }
    }
    else if (rc != CURLE_OK)
    {
        fprintf(stderr, "  [stt error: %s]\n", curl_easy_strerror(rc));
    }
    else
    {
        fprintf(stderr, "  [stt error: HTTP %ld]\n", status);
    }

    free(resp.data);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    free(wav);
    return ok;
}

void http_reset(const char *shim_url, const char *session, long timeout_ms)
{
    char esc_session[256];
    json_escape(session, esc_session, sizeof esc_session);
    char body[320];
    int bodylen = snprintf(body, sizeof body, "{\"session\":\"%s\"}", esc_session);

    char url[256];
    snprintf(url, sizeof url, "%s/v1/reset", shim_url);

    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "  [reset failed: curl_easy_init]\n"); return; }

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) bodylen);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
    {
        fprintf(stderr, "  [reset failed: %s]\n", curl_easy_strerror(rc));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Reassembles the SSE byte stream into lines as curl delivers chunks, and dispatches each
// `data:` line to the caller's sentence callback -- same line-reassembly approach as
// src/firmware/main/net.c's net_respond(), adapted to curl's push-callback shape instead of a
// pull-style read loop.
struct sse_ctx
{
    char          line[1024];
    int           linelen;
    sentence_cb_t cb;
    void         *user_ctx;
    bool          done;
    bool          cb_failed;
};

static size_t sse_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct sse_ctx *s = userdata;
    size_t n = size * nmemb;
    for (size_t i = 0; i < n && !s->done; i++)
    {
        char ch = ptr[i];
        if (ch == '\n')
        {
            s->line[s->linelen] = '\0';
            s->linelen = 0;
            if (strncmp(s->line, "data:", 5) != 0) continue;
            const char *pl = s->line + 5;
            while (*pl == ' ') pl++;
            if (strcmp(pl, "[DONE]") == 0)
            {
                s->done = true;
            }
            else
            {
                char sentence[512];
                if (json_get_string(pl, "sentence", sentence, sizeof sentence))
                {
                    char voice[32];
                    bool has_voice = json_get_string(pl, "voice", voice, sizeof voice);
                    if (!s->cb(sentence, has_voice ? voice : NULL, s->user_ctx))
                    {
                        s->cb_failed = true;
                        s->done = true;
                    }
                }
                else if (json_get_string(pl, "error", sentence, sizeof sentence))
                {
                    fprintf(stderr, "  [shim error: %s]\n", sentence);
                }
            }
        }
        else if (ch != '\r' && s->linelen < (int) sizeof(s->line) - 1)
        {
            s->line[s->linelen++] = ch;
        }
    }
    // Returning short of n once done/failed deliberately aborts the transfer (CURLE_WRITE_ERROR)
    // instead of reading out the rest of the reply for nothing.
    return s->done ? 0 : n;
}

bool http_respond_stream(const char *shim_url, const char *session, const char *text,
                          sentence_cb_t on_sentence, void *ctx, long timeout_ms)
{
    size_t esc_cap = strlen(text) * 2 + 1;
    char *esc_text = malloc(esc_cap);
    if (!esc_text) return false;
    json_escape(text, esc_text, esc_cap);

    char esc_session[256];
    json_escape(session, esc_session, sizeof esc_session);

    size_t body_cap = esc_cap + strlen(esc_session) + 64;
    char *body = malloc(body_cap);
    if (!body) { free(esc_text); return false; }
    int bodylen = snprintf(body, body_cap, "{\"session\":\"%s\",\"text\":\"%s\"}", esc_session, esc_text);
    free(esc_text);

    char url[256];
    snprintf(url, sizeof url, "%s/v1/respond", shim_url);

    struct sse_ctx sctx = { .cb = on_sentence, .user_ctx = ctx };

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return false; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) bodylen);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    bool ok;
    if (sctx.cb_failed)
    {
        ok = false;
    }
    else if (sctx.done)
    {
        ok = true;   // saw [DONE] -- possibly via our own early-abort write return
    }
    else if (rc == CURLE_OK && status == 200)
    {
        ok = true;
    }
    else
    {
        ok = false;
        if (rc != CURLE_OK) fprintf(stderr, "  [shim error: %s]\n", curl_easy_strerror(rc));
        else fprintf(stderr, "  [shim error: HTTP %ld]\n", status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    return ok;
}

bool http_tts(const char *tts_url, const char *voice, const char *sentence,
              uint8_t **out_wav, size_t *out_len, long timeout_ms)
{
    *out_wav = NULL;
    *out_len = 0;

    size_t esc_cap = strlen(sentence) * 2 + 1;
    char *esc = malloc(esc_cap);
    if (!esc) return false;
    json_escape(sentence, esc, esc_cap);

    size_t body_cap = esc_cap + strlen(voice) + 96;
    char *body = malloc(body_cap);
    if (!body) { free(esc); return false; }
    int bodylen = snprintf(body, body_cap,
        "{\"model\":\"kokoro\",\"input\":\"%s\",\"voice\":\"%s\",\"response_format\":\"wav\"}", esc, voice);
    free(esc);

    char url[256];
    snprintf(url, sizeof url, "%s/v1/audio/speech", tts_url);

    struct membuf resp = { 0 };
    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return false; }

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) bodylen);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = (rc == CURLE_OK && status == 200 && resp.data);

    if (ok)
    {
        *out_wav = resp.data;
        *out_len = resp.len;
    }
    else
    {
        if (rc != CURLE_OK) fprintf(stderr, "  [tts error: %s]\n", curl_easy_strerror(rc));
        else fprintf(stderr, "  [tts error: HTTP %ld]\n", status);
        free(resp.data);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    return ok;
}
