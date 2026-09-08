/*
 * TSM - Temporal Signal Medium
 * Minimal PCM WAV reader and writer.
 *
 * Copyright (c) 2026 Francesco De Simone
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. A copy is in LICENSE
 * beside this file, and at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Redistributions and derivative works must carry the attribution in NOTICE.
 */

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
