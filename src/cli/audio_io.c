#include "audio_io.h"

#include <portaudio.h>
#include <soxr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RECORD_SECONDS 300   // safety cap; well beyond any real utterance
#define PLAY_CHUNK_FRAMES  1024  // bounds cancellation latency while writing to PortAudio

static PaStream *s_output_stream = NULL;
static int       s_output_rate = 0;

static int16_t *s_rec_buf = NULL;
static size_t   s_rec_cap = 0;
static volatile size_t s_rec_count = 0;

// Runs on PortAudio's audio thread while recording; only ever active between
// audio_record_utterance()'s Pa_StartStream and Pa_StopStream, so it's the sole writer of
// s_rec_count during that window.
static int record_cb(const void *input, void *output, unsigned long frames,
                      const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags,
                      void *user_data)
{
    (void) output; (void) time_info; (void) status_flags; (void) user_data;
    if (!input) return paContinue;
    size_t avail = s_rec_cap - s_rec_count;
    size_t n = frames < avail ? frames : avail;
    if (n > 0)
    {
        memcpy(s_rec_buf + s_rec_count, input, n * sizeof(int16_t));
        s_rec_count += n;
    }
    return paContinue;
}

bool audio_init(void)
{
    PaError err = Pa_Initialize();
    if (err != paNoError)
    {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return false;
    }

    PaDeviceIndex out_dev = Pa_GetDefaultOutputDevice();
    if (out_dev == paNoDevice)
    {
        fprintf(stderr, "audio: no default output device\n");
        Pa_Terminate();
        return false;
    }
    const PaDeviceInfo *out_info = Pa_GetDeviceInfo(out_dev);
    s_output_rate = (int) out_info->defaultSampleRate;

    PaStreamParameters out_params = {
        .device = out_dev,
        .channelCount = 1,
        .sampleFormat = paInt16,
        .suggestedLatency = out_info->defaultLowOutputLatency,
        .hostApiSpecificStreamInfo = NULL,
    };
    err = Pa_OpenStream(&s_output_stream, NULL, &out_params, s_output_rate,
                         paFramesPerBufferUnspecified, paNoFlag, NULL, NULL);
    if (err != paNoError)
    {
        fprintf(stderr, "audio: output stream open failed: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return false;
    }
    err = Pa_StartStream(s_output_stream);
    if (err != paNoError)
    {
        fprintf(stderr, "audio: output stream start failed: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(s_output_stream);
        s_output_stream = NULL;
        Pa_Terminate();
        return false;
    }

    s_rec_cap = (size_t) AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS;
    s_rec_buf = malloc(s_rec_cap * sizeof(int16_t));
    if (!s_rec_buf)
    {
        fprintf(stderr, "audio: failed to allocate %zu-sample record buffer\n", s_rec_cap);
        Pa_StopStream(s_output_stream);
        Pa_CloseStream(s_output_stream);
        s_output_stream = NULL;
        Pa_Terminate();
        return false;
    }

    return true;
}

void audio_shutdown(void)
{
    if (s_output_stream)
    {
        Pa_StopStream(s_output_stream);
        Pa_CloseStream(s_output_stream);
        s_output_stream = NULL;
    }
    free(s_rec_buf);
    s_rec_buf = NULL;
    Pa_Terminate();
}

audio_record_result_t audio_record_utterance(int16_t **out_audio, size_t *out_count,
                                             cancel_check_t cancel, const volatile void *cancel_ctx)
{
    *out_audio = NULL;
    *out_count = 0;

    printf("\n[Enter] to start talking... ");
    fflush(stdout);
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {}
    if (cancel_requested(cancel, cancel_ctx)) return AUDIO_RECORD_INTERRUPTED;

    PaDeviceIndex in_dev = Pa_GetDefaultInputDevice();
    if (in_dev == paNoDevice)
    {
        fprintf(stderr, "audio: no default input device\n");
        return AUDIO_RECORD_ERROR;
    }
    const PaDeviceInfo *in_info = Pa_GetDeviceInfo(in_dev);
    PaStreamParameters in_params = {
        .device = in_dev,
        .channelCount = 1,
        .sampleFormat = paInt16,
        .suggestedLatency = in_info->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = NULL,
    };

    s_rec_count = 0;
    PaStream *input_stream = NULL;
    PaError err = Pa_OpenStream(&input_stream, &in_params, NULL, AUDIO_SAMPLE_RATE,
                                 paFramesPerBufferUnspecified, paNoFlag, record_cb, NULL);
    if (err != paNoError)
    {
        fprintf(stderr, "audio: record open failed: %s\n", Pa_GetErrorText(err));
        return AUDIO_RECORD_ERROR;
    }
    err = Pa_StartStream(input_stream);
    if (err != paNoError)
    {
        fprintf(stderr, "audio: record start failed: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(input_stream);
        return AUDIO_RECORD_ERROR;
    }

    printf("\U0001F3A4 recording -- [Enter] to stop... ");
    fflush(stdout);
    while ((ch = getchar()) != '\n' && ch != EOF) {}

    Pa_StopStream(input_stream);
    Pa_CloseStream(input_stream);

    if (cancel_requested(cancel, cancel_ctx)) return AUDIO_RECORD_INTERRUPTED;
    if (s_rec_count == 0) return AUDIO_RECORD_EMPTY;

    int16_t *out = malloc(s_rec_count * sizeof(int16_t));
    if (!out) return AUDIO_RECORD_ERROR;
    memcpy(out, s_rec_buf, s_rec_count * sizeof(int16_t));
    *out_audio = out;
    *out_count = s_rec_count;
    return AUDIO_RECORD_OK;
}

void audio_play(const int16_t *audio, size_t count, int sample_rate,
                cancel_check_t cancel, const volatile void *cancel_ctx)
{
    if (!audio || count == 0 || !s_output_stream) return;

    const int16_t *to_play = audio;
    size_t to_play_count = count;
    int16_t *resampled = NULL;

    if (sample_rate != s_output_rate)
    {
        size_t out_cap = (size_t) ((double) count * s_output_rate / sample_rate) + 16;
        resampled = malloc(out_cap * sizeof(int16_t));
        if (!resampled) return;

        soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
        soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, 0);   // match python soxr's default
        size_t idone = 0, odone = 0;
        soxr_error_t err = soxr_oneshot(sample_rate, s_output_rate, 1,
                                         audio, count, &idone,
                                         resampled, out_cap, &odone,
                                         &io_spec, &quality, NULL);
        if (err)
        {
            fprintf(stderr, "audio: resample failed: %s\n", err);
            free(resampled);
            return;
        }
        to_play = resampled;
        to_play_count = odone;
    }

    size_t offset = 0;
    while (offset < to_play_count && !cancel_requested(cancel, cancel_ctx))
    {
        size_t chunk = to_play_count - offset;
        if (chunk > PLAY_CHUNK_FRAMES) chunk = PLAY_CHUNK_FRAMES;
        PaError perr = Pa_WriteStream(s_output_stream, to_play + offset, chunk);
        if (perr != paNoError && perr != paOutputUnderflowed)
        {
            fprintf(stderr, "audio: playback failed: %s\n", Pa_GetErrorText(perr));
            break;
        }
        offset += chunk;
    }

    free(resampled);
}
