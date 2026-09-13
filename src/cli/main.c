// Billy Bass -- CLI reference client (thin fish client), C port.
//
// Exercises the full backend voice pipeline from any host with a mic + speaker, so the
// STT -> brain -> TTS chain is validated end-to-end before any ESP32 exists. This client *is*
// the documented fish<->backend protocol contract that the ESP32 firmware reimplements
// (src/firmware/main/net.c).
//
// Turn pipeline: record -> STT -> shim (streams sentences) -> TTS per sentence -> play, with
// sentence 1 spoken while later sentences are still being generated/synthesized. A synth thread
// reads the shim's SSE stream and calls TTS one sentence at a time, handing finished audio to
// the main thread over a queue for playback -- the main thread plays while the worker keeps
// pulling the next sentence ahead of it.
#include "audio_io.h"
#include "http.h"
#include "wav.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_VOICE       "am_onyx"   // used only if the shim omits "voice" and --voice wasn't given
#define STT_TIMEOUT_MS      60000
#define RESPOND_TIMEOUT_MS  120000
#define TTS_TIMEOUT_MS      120000
#define RESET_TIMEOUT_MS    10000

// main() owns the process signal disposition, so the handler and its flag stay here.
volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig)
{
    (void) sig;
    g_interrupted = 1;   // async-signal-safe: sig_atomic_t write only, no I/O or allocation here
}

static bool interrupted(const volatile void *ctx)
{
    const volatile sig_atomic_t *flag = ctx;
    return *flag != 0;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// --- Playback queue: the worker thread pushes finished (sentence, audio) pairs as TTS synth
// completes; the main thread pops and plays them in order, one turn's worth at a time. ---------

typedef struct pb_item
{
    char           *sentence;
    int16_t        *audio;
    size_t          count;
    int             sample_rate;
    double          t_sent;   // TTS request sent, for the "sent->audio" latency print
    struct pb_item *next;
} pb_item_t;

typedef struct
{
    pb_item_t      *head, *tail;
    bool            done;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} pb_queue_t;

static void pb_queue_init(pb_queue_t *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static void pb_queue_destroy(pb_queue_t *q)
{
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
}

static void pb_queue_push(pb_queue_t *q, pb_item_t *item)
{
    pthread_mutex_lock(&q->mu);
    item->next = NULL;
    if (q->tail) q->tail->next = item; else q->head = item;
    q->tail = item;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static void pb_queue_finish(pb_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->done = true;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

// Blocks until an item is available or the queue is finished-and-drained (returns NULL then).
static pb_item_t *pb_queue_pop(pb_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->done) pthread_cond_wait(&q->cv, &q->mu);
    pb_item_t *item = q->head;
    if (item)
    {
        q->head = item->next;
        if (!q->head) q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mu);
    return item;
}

// --- Synth worker: reads the shim's SSE stream and, for each sentence, synthesizes it via TTS
// before moving to the next -- same one-sentence-at-a-time pipelining as the Python reference's
// synth_worker() iterating its SSE generator. ----------------------------------------------

typedef struct
{
    const char *tts_url;
    const char *voice_override;
    pb_queue_t *queue;

    const char *shim_url;
    const char *session;
    const char *text;
    const char *language;
    cancel_check_t cancel;
    const volatile void *cancel_ctx;

    double t_prompt_sent;
    bool   first_sentence_seen;
    double ttfs;   // prompt-sent -> first-sentence latency (s)
} synth_ctx_t;

static bool on_sentence(const char *sentence, const char *voice, void *ctx_)
{
    synth_ctx_t *ctx = ctx_;
    if (cancel_requested(ctx->cancel, ctx->cancel_ctx)) return false;
    if (!ctx->first_sentence_seen)
    {
        ctx->ttfs = now_seconds() - ctx->t_prompt_sent;
        ctx->first_sentence_seen = true;
    }

    const char *chosen_voice = ctx->voice_override ? ctx->voice_override
                              : voice              ? voice
                                                    : DEFAULT_VOICE;

    double t_sent = now_seconds();
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    if (!http_tts(ctx->tts_url, chosen_voice, sentence, &wav, &wav_len, TTS_TIMEOUT_MS,
                  ctx->cancel, ctx->cancel_ctx))
    {
        return !cancel_requested(ctx->cancel, ctx->cancel_ctx);
    }

    wav_pcm16_t pcm;
    bool parsed = wav_parse_pcm16(wav, wav_len, &pcm);
    free(wav);
    if (!parsed)
    {
        fprintf(stderr, "  [tts error: unparseable wav response]\n");
        return true;
    }

    pb_item_t *item = malloc(sizeof *item);
    item->sentence = xstrdup(sentence);
    item->audio = pcm.samples;
    item->count = pcm.count;
    item->sample_rate = (int) pcm.sample_rate;
    item->t_sent = t_sent;
    pb_queue_push(ctx->queue, item);
    return true;
}

static void *synth_worker(void *arg)
{
    synth_ctx_t *ctx = arg;
    ctx->t_prompt_sent = now_seconds();
    http_respond_stream(ctx->shim_url, ctx->session, ctx->text, ctx->language, on_sentence, ctx,
                        RESPOND_TIMEOUT_MS, ctx->cancel, ctx->cancel_ctx);
    pb_queue_finish(ctx->queue);
    return NULL;
}

// One full turn: STT -> shim (streamed) -> TTS per sentence -> play, printing the transcript and
// latencies. Shared by the interactive mic loop and --input-wav's single-shot mode; `pcm` is
// already-captured audio either way (a mic recording or a parsed WAV file), so this starts at STT.
static void run_turn(const char *shim_url, const char *stt_url, const char *tts_url,
                      const char *session, const char *voice_override,
                      const int16_t *pcm, size_t count, uint32_t sample_rate)
{
    double t_end = now_seconds();

    char text[2048];
    char language[64];
    bool stt_ok = http_stt(stt_url, pcm, count, sample_rate, text, sizeof text,
                           language, sizeof language, STT_TIMEOUT_MS,
                           interrupted, &g_interrupted);
    double t_stt = now_seconds();
    if (g_interrupted) return;
    if (!stt_ok || text[0] == '\0')
    {
        printf("  (heard nothing)\n");
        return;
    }
    printf("  \U0001F5E3  %s  [%s]\n", text, language);

    pb_queue_t queue;
    pb_queue_init(&queue);
    synth_ctx_t ctx = {
        .tts_url = tts_url,
        .voice_override = voice_override,
        .queue = &queue,
        .shim_url = shim_url,
        .session = session,
        .text = text,
        .language = language,
        .cancel = interrupted,
        .cancel_ctx = &g_interrupted,
    };

    pthread_t worker;
    pthread_create(&worker, NULL, synth_worker, &ctx);

    bool first = true;
    double t_first_audio = t_end;
    pb_item_t *item;
    while ((item = pb_queue_pop(&queue)) != NULL)
    {
        if (first)
        {
            t_first_audio = now_seconds();
            first = false;
        }
        double t_play = now_seconds();
        printf("  \U0001F41F %s  (sent→audio %.2fs)\n", item->sentence, t_play - item->t_sent);
        audio_play(item->audio, item->count, item->sample_rate, interrupted, &g_interrupted);
        free(item->sentence);
        free(item->audio);
        free(item);
    }
    pthread_join(worker, NULL);
    pb_queue_destroy(&queue);

    double ttfs = ctx.first_sentence_seen ? ctx.ttfs : NAN;
    printf("  ⏱  STT %.2fs · LLM ttfs %.2fs · end→first-audio %.2fs\n",
           t_stt - t_end, ttfs, t_first_audio - t_end);
}

// Reads a whole file into a malloc'd buffer (caller frees). Returns NULL on any error.
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t) len);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t) len, f);
    fclose(f);
    if (rd != (size_t) len) { free(buf); return NULL; }
    *out_len = (size_t) len;
    return buf;
}

// --- CLI args -------------------------------------------------------------------------------

typedef struct
{
    const char *host;
    const char *shim_url;
    const char *stt_url;
    const char *tts_url;
    const char *voice;
    const char *session;
    const char *input_wav;
} cli_args_t;

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --host <backend-host> [options]\n"
        "       %s --shim-url <url> --stt-url <url> --tts-url <url> [options]\n"
        "\n"
        "  --host <hostname/IP>   backend host; builds shim(:8000)/STT(:8081)/TTS(:8880) URLs\n"
        "  --shim-url <url>       override, e.g. http://<backend-host>:8000\n"
        "  --stt-url <url>        override\n"
        "  --tts-url <url>        override\n"
        "  --voice <voice>        override the shim's per-sentence voice for the whole session\n"
        "  --session <id>         shim conversation id (default: \"default\")\n"
        "  --input-wav <path>     skip the mic: run one turn on this WAV file (16 kHz mono,\n"
        "                         matching what the mic path itself records) and exit --\n"
        "                         for replaying the same utterance repeatedly\n",
        prog, prog);
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc)
    {
        fprintf(stderr, "%s: %s requires a value\n", argv[0], argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

int main(int argc, char **argv)
{
    cli_args_t args = { .session = "default" };
    for (int i = 1; i < argc; i++)
    {
        if      (!strcmp(argv[i], "--host"))     args.host = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--shim-url")) args.shim_url = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--stt-url"))  args.stt_url = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--tts-url"))  args.tts_url = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--voice"))    args.voice = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--session"))  args.session = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--input-wav")) args.input_wav = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "%s: unrecognized argument: %s\n", argv[0], argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    char shim_url[256] = "", stt_url[256] = "", tts_url[256] = "";
    if (args.shim_url) snprintf(shim_url, sizeof shim_url, "%s", args.shim_url);
    else if (args.host) snprintf(shim_url, sizeof shim_url, "http://%s:8000", args.host);
    if (args.stt_url) snprintf(stt_url, sizeof stt_url, "%s", args.stt_url);
    else if (args.host) snprintf(stt_url, sizeof stt_url, "http://%s:8081", args.host);
    if (args.tts_url) snprintf(tts_url, sizeof tts_url, "%s", args.tts_url);
    else if (args.host) snprintf(tts_url, sizeof tts_url, "http://%s:8880", args.host);

    if (!shim_url[0] || !stt_url[0] || !tts_url[0])
    {
        fprintf(stderr, "%s: provide --host (backend hostname/IP), or override each of "
                         "--shim-url/--stt-url/--tts-url\n", argv[0]);
        print_usage(argv[0]);
        return 2;
    }

    char voice_note[128];
    if (args.voice) snprintf(voice_note, sizeof voice_note, "voice override %s", args.voice);
    else snprintf(voice_note, sizeof voice_note, "shim-chosen voice (fallback %s)", DEFAULT_VOICE);
    printf("Billy CLI -- shim %s · STT %s · TTS %s · %s\n", shim_url, stt_url, tts_url, voice_note);
    fflush(stdout);

    // Deliberately no SA_RESTART: a Ctrl-C during either "press Enter" wait in
    // audio_record_utterance() needs the blocked getchar() to unblock immediately (via EINTR)
    // rather than transparently resuming the read as if nothing happened.
    struct sigaction sa = { .sa_handler = handle_sigint, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0)
    {
        perror("sigaction(SIGINT)");
        return 1;
    }

    http_global_init();
    if (!audio_init())
    {
        http_global_cleanup();
        return 1;
    }

    http_reset(shim_url, args.session, RESET_TIMEOUT_MS, interrupted, &g_interrupted);

    if (args.input_wav)
    {
        size_t file_len = 0;
        uint8_t *file_buf = read_file(args.input_wav, &file_len);
        if (!file_buf)
        {
            fprintf(stderr, "%s: couldn't read %s\n", argv[0], args.input_wav);
            audio_shutdown();
            http_global_cleanup();
            return 1;
        }
        wav_pcm16_t pcm;
        bool parsed = wav_parse_pcm16(file_buf, file_len, &pcm);
        free(file_buf);
        if (!parsed)
        {
            fprintf(stderr, "%s: %s is not a supported PCM16 WAV\n", argv[0], args.input_wav);
            audio_shutdown();
            http_global_cleanup();
            return 1;
        }
        run_turn(shim_url, stt_url, tts_url, args.session, args.voice,
                 pcm.samples, pcm.count, pcm.sample_rate);
        free(pcm.samples);
        audio_shutdown();
        http_global_cleanup();
        return 0;
    }

    printf("Ctrl-C to quit.\n");
    fflush(stdout);

    for (;;)
    {
        if (g_interrupted) break;   // Ctrl-C arrived during the previous turn's network activity
                                     // rather than a keypress wait -- don't start another one

        size_t rec_count = 0;
        int16_t *rec = NULL;
        audio_record_result_t record_result = audio_record_utterance(
            &rec, &rec_count, interrupted, &g_interrupted);
        if (record_result != AUDIO_RECORD_OK)
        {
            if (record_result == AUDIO_RECORD_INTERRUPTED) break;
            if (feof(stdin)) break;     // stdin closed (e.g. piped/non-interactive) -- stop spinning
            continue;
        }

        run_turn(shim_url, stt_url, tts_url, args.session, args.voice,
                 rec, rec_count, AUDIO_SAMPLE_RATE);
        free(rec);
    }

    if (g_interrupted)
    {
        printf("\nbye \U0001F41F\n");
        fflush(stdout);
    }
    audio_shutdown();
    http_global_cleanup();
    return 0;
}
