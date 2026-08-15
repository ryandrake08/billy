#include "wav.h"
#include <stdlib.h>
#include <string.h>

static void wr_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static uint16_t rd_le16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

void wav_write_header(uint8_t out[WAV_HEADER_BYTES], uint32_t nsamples, uint32_t rate)
{
    uint32_t data_bytes = nsamples * 2;
    memcpy(out, "RIFF", 4);          wr_le32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);     wr_le32(out + 16, 16);
    wr_le16(out + 20, 1);            wr_le16(out + 22, 1);            // PCM, mono
    wr_le32(out + 24, rate);         wr_le32(out + 28, rate * 2);     // sample rate, byte rate
    wr_le16(out + 32, 2);            wr_le16(out + 34, 16);           // block align, bits
    memcpy(out + 36, "data", 4);     wr_le32(out + 40, data_bytes);
}

bool wav_parse_pcm16(const uint8_t *buf, size_t len, wav_pcm16_t *out)
{
    out->samples = NULL;
    out->count = 0;
    out->sample_rate = 0;

    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
    {
        return false;
    }

    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0, format = 0;
    size_t data_off = 0, data_size = 0, off = 12;
    while (off + 8 <= len)
    {
        uint32_t csize = rd_le32(buf + off + 4);
        if (memcmp(buf + off, "fmt ", 4) == 0 && off + 8 + 16 <= len)
        {
            format   = rd_le16(buf + off + 8 + 0);
            channels = rd_le16(buf + off + 8 + 2);
            rate     = rd_le32(buf + off + 8 + 4);
            bits     = rd_le16(buf + off + 8 + 14);
        }
        else if (memcmp(buf + off, "data", 4) == 0)
        {
            data_off = off + 8;
            data_size = csize;
            break;
        }
        off += 8 + csize + (csize & 1);   // chunks are word-aligned
    }

    if (data_off == 0 || format != 1 || bits != 16 || (channels != 1 && channels != 2))
    {
        return false;
    }
    if (data_size > len - data_off) data_size = len - data_off;   // clamp to bytes actually present

    const int16_t *src = (const int16_t *) (buf + data_off);
    size_t nframes = data_size / (2 * (size_t) channels);
    int16_t *pcm = malloc(nframes * sizeof(int16_t));
    if (!pcm) return false;

    if (channels == 1)
    {
        memcpy(pcm, src, nframes * sizeof(int16_t));
    }
    else   // downmix stereo -> mono
    {
        for (size_t i = 0; i < nframes; i++)
            pcm[i] = (int16_t) (((int) src[2 * i] + src[2 * i + 1]) / 2);
    }

    out->samples = pcm;
    out->count = nframes;
    out->sample_rate = rate;
    return true;
}
