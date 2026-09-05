#include "wav_io_simple.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_le16_from_memory(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32_from_memory(const uint8_t *p) {
    return
        ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static void write_le16(FILE *file, uint16_t value) {
    fputc((int)(value & 0xff), file);
    fputc((int)((value >> 8) & 0xff), file);
}

static void write_le32(FILE *file, uint32_t value) {
    fputc((int)(value & 0xff), file);
    fputc((int)((value >> 8) & 0xff), file);
    fputc((int)((value >> 16) & 0xff), file);
    fputc((int)((value >> 24) & 0xff), file);
}

int wav_load_pcm16_mono_simple(const char *path, WavData *wav) {
    FILE *file;
    uint8_t riff_header[12];

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;

    uint8_t *data = NULL;
    uint32_t data_size = 0;

    uint64_t frame_count;
    double *samples;
    uint64_t i;

    memset(wav, 0, sizeof(*wav));

    file = fopen(path, "rb");

    if (!file) {
        fprintf(stderr, "Cannot open WAV: %s\n", path);
        return 0;
    }

    if (
        fread(riff_header, 1, 12, file) != 12 ||
        memcmp(riff_header, "RIFF", 4) != 0 ||
        memcmp(riff_header + 8, "WAVE", 4) != 0
    ) {
        fprintf(stderr, "Invalid WAV header\n");
        fclose(file);
        return 0;
    }

    while (!feof(file)) {
        uint8_t chunk_header[8];
        uint32_t chunk_size;

        if (fread(chunk_header, 1, 8, file) != 8) {
            break;
        }

        chunk_size = read_le32_from_memory(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t *fmt = (uint8_t *)malloc(chunk_size);

            if (!fmt) {
                fclose(file);
                return 0;
            }

            if (fread(fmt, 1, chunk_size, file) != chunk_size) {
                free(fmt);
                fclose(file);
                return 0;
            }

            audio_format = read_le16_from_memory(fmt + 0);
            channels = read_le16_from_memory(fmt + 2);
            sample_rate = read_le32_from_memory(fmt + 4);
            bits_per_sample = read_le16_from_memory(fmt + 14);

            free(fmt);

        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data = (uint8_t *)malloc(chunk_size);

            if (!data) {
                fclose(file);
                return 0;
            }

            if (fread(data, 1, chunk_size, file) != chunk_size) {
                free(data);
                fclose(file);
                return 0;
            }

            data_size = chunk_size;
            break;

        } else {
            fseek(file, (long)chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1) {
            fseek(file, 1, SEEK_CUR);
        }
    }

    fclose(file);

    if (!data) {
        fprintf(stderr, "WAV has no data chunk\n");
        return 0;
    }

    if (audio_format != 1 || bits_per_sample != 16 || channels == 0) {
        fprintf(stderr, "Only PCM 16-bit WAV is supported\n");
        free(data);
        return 0;
    }

    frame_count = data_size / ((uint32_t)channels * 2u);
    samples = (double *)calloc((size_t)frame_count, sizeof(double));

    if (!samples) {
        free(data);
        return 0;
    }

    for (i = 0; i < frame_count; ++i) {
        int32_t accumulator = 0;
        uint16_t channel;

        for (channel = 0; channel < channels; ++channel) {
            uint64_t offset =
                i * (uint64_t)channels * 2u +
                (uint64_t)channel * 2u;

            int16_t sample =
                (int16_t)(data[offset] | ((uint16_t)data[offset + 1] << 8));

            accumulator += sample;
        }

        samples[i] =
            ((double)accumulator / (double)channels) / 32768.0;
    }

    free(data);

    wav->sample_rate = sample_rate;
    wav->channels = channels;
    wav->sample_count = frame_count;
    wav->samples = samples;

    return 1;
}

void wav_free_simple(WavData *wav) {
    free(wav->samples);
    memset(wav, 0, sizeof(*wav));
}

int wav_write_pcm16_mono_simple(
    const char *path,
    const int16_t *samples,
    uint64_t sample_count,
    uint32_t sample_rate
) {
    FILE *file;
    uint32_t data_size;
    uint32_t riff_size;

    if (sample_count > 0x7fffffffULL) {
        fprintf(stderr, "WAV too large for simple writer\n");
        return 0;
    }

    file = fopen(path, "wb");

    if (!file) {
        fprintf(stderr, "Cannot write WAV: %s\n", path);
        return 0;
    }

    data_size = (uint32_t)(sample_count * 2u);
    riff_size = 36u + data_size;

    fwrite("RIFF", 1, 4, file);
    write_le32(file, riff_size);
    fwrite("WAVE", 1, 4, file);

    fwrite("fmt ", 1, 4, file);
    write_le32(file, 16);
    write_le16(file, 1);
    write_le16(file, 1);
    write_le32(file, sample_rate);
    write_le32(file, sample_rate * 2u);
    write_le16(file, 2);
    write_le16(file, 16);

    fwrite("data", 1, 4, file);
    write_le32(file, data_size);
    fwrite(samples, 2, (size_t)sample_count, file);

    fclose(file);

    return 1;
}
