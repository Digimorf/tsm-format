#ifndef WAV_IO_SIMPLE_H
#define WAV_IO_SIMPLE_H

#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint64_t sample_count;
    double *samples;
} WavData;

int wav_load_pcm16_mono_simple(const char *path, WavData *wav);
void wav_free_simple(WavData *wav);

int wav_write_pcm16_mono_simple(
    const char *path,
    const int16_t *samples,
    uint64_t sample_count,
    uint32_t sample_rate
);

#endif
