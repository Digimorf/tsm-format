/*
 * TSM - Temporal Signal Medium
 * TSM v5.2 to WAV: the reference renderer.
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

#include "tsm_v5.h"
#include "wav_io_simple.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    tsm2wav_v5.c

    Generic TSM v5.1 -> WAV renderer.

    Supports:
        - HOLD/SILENCE
        - ZERO_SILENCE
        - INDEXED_DELTA with escape-byte raw delta encoding
        - old nibble-packed indexed regions for backward inspection
*/

typedef struct {
    int16_t *samples;
    uint64_t count;
    uint64_t capacity;
} AudioBuffer;

static void print_u64(uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    printf("%s", text);
}

static void audio_init(AudioBuffer *audio) {
    audio->samples = NULL;
    audio->count = 0;
    audio->capacity = 0;
}

static void audio_free(AudioBuffer *audio) {
    free(audio->samples);
    memset(audio, 0, sizeof(*audio));
}

static void audio_reserve(AudioBuffer *audio, uint64_t required) {
    uint64_t new_capacity;

    if (required <= audio->capacity) {
        return;
    }

    new_capacity = audio->capacity ? audio->capacity * 2 : 65536;

    while (new_capacity < required) {
        new_capacity *= 2;
    }

    audio->samples =
        (int16_t *)realloc(
            audio->samples,
            (size_t)new_capacity * sizeof(int16_t)
        );

    if (!audio->samples) {
        fprintf(stderr, "Out of memory while growing audio buffer\n");
        exit(1);
    }

    audio->capacity = new_capacity;
}

static void audio_append(AudioBuffer *audio, int16_t value, uint64_t sample_count) {
    uint64_t i;

    if (sample_count == 0) {
        return;
    }

    audio_reserve(audio, audio->count + sample_count);

    for (i = 0; i < sample_count; ++i) {
        audio->samples[audio->count++] = value;
    }
}

static void audio_trim(AudioBuffer *audio, uint64_t target_count) {
    if (audio->count > target_count) {
        audio->count = target_count;
    }
}

static uint8_t read_nibble_index(const uint8_t *payload, uint64_t index) {
    uint8_t byte = payload[index >> 1];

    if ((index & 1) == 0) {
        return byte & 0x0f;
    }

    return (byte >> 4) & 0x0f;
}

static uint8_t *read_payload(FILE *file, uint64_t offset, uint64_t size) {
    uint8_t *payload = (uint8_t *)malloc((size_t)size);

    if (!payload) {
        fprintf(stderr, "Out of memory while reading payload\n");
        exit(1);
    }

    fseek(file, (long)offset, SEEK_SET);

    if (fread(payload, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "Cannot read indexed payload\n");
        free(payload);
        exit(1);
    }

    return payload;
}

static uint32_t read_u32_le_from_memory(const uint8_t *data, uint64_t offset) {
    return
        ((uint32_t)data[offset]) |
        ((uint32_t)data[offset + 1] << 8) |
        ((uint32_t)data[offset + 2] << 16) |
        ((uint32_t)data[offset + 3] << 24);
}

static uint32_t *read_delta_table(FILE *file, const TSM5Region *region) {
    uint32_t *table;
    uint32_t i;

    table =
        (uint32_t *)calloc(
            region->table_count,
            sizeof(uint32_t)
        );

    if (!table) {
        fprintf(stderr, "Out of memory while reading delta table\n");
        exit(1);
    }

    fseek(file, (long)region->aux_offset, SEEK_SET);

    for (i = 0; i < region->table_count; ++i) {
        uint32_t value = 0;

        if (!tsm5_read_u32(file, &value)) {
            fprintf(stderr, "Cannot read delta table\n");
            free(table);
            exit(1);
        }

        table[i] = value;
    }

    return table;
}

static uint64_t absolute_tick_to_sample(
    uint64_t absolute_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
) {
    return
        tsm5_ticks_to_samples(
            absolute_ticks,
            time_unit_ns,
            sample_rate
        );
}

static uint64_t expected_output_samples_from_header(
    const TSM5Header *header,
    uint32_t output_sample_rate
) {
    if (header->reserved1 != 0 && header->reserved2 != 0) {
        long double samples =
            ((long double)header->reserved1 *
             (long double)output_sample_rate) /
            (long double)header->reserved2;

        return (uint64_t)(samples + 0.5L);
    }

    return
        tsm5_ticks_to_samples(
            header->total_duration_ticks,
            header->base_time_unit_ns,
            output_sample_rate
        );
}

static void render_until_absolute_sample(
    AudioBuffer *audio,
    uint64_t target_sample,
    int16_t value
) {
    if (target_sample <= audio->count) {
        return;
    }

    audio_append(
        audio,
        value,
        target_sample - audio->count
    );
}

static void render_hold_until(
    AudioBuffer *audio,
    uint64_t target_sample,
    int16_t amplitude,
    int state
) {
    render_until_absolute_sample(
        audio,
        target_sample,
        state > 0 ? amplitude : (int16_t)-amplitude
    );
}

static void render_region_gap_as_hold(
    AudioBuffer *audio,
    const TSM5Region *region,
    uint32_t sample_rate,
    int16_t amplitude,
    int state
) {
    uint64_t region_start_sample =
        absolute_tick_to_sample(
            region->start_ticks,
            region->time_unit_ns,
            sample_rate
        );

    render_hold_until(
        audio,
        region_start_sample,
        amplitude,
        state
    );
}

static void render_silence_region(
    AudioBuffer *audio,
    const TSM5Region *region,
    uint32_t sample_rate,
    int16_t amplitude,
    int *state
) {
    uint64_t region_end_sample;

    if (region->reserved0 == 0 || region->reserved0 == 1) {
        *state = region->reserved0 ? 1 : -1;
    }

    render_region_gap_as_hold(
        audio,
        region,
        sample_rate,
        amplitude,
        *state
    );

    region_end_sample =
        absolute_tick_to_sample(
            region->start_ticks + region->duration_ticks,
            region->time_unit_ns,
            sample_rate
        );

    render_hold_until(
        audio,
        region_end_sample,
        amplitude,
        *state
    );
}

static void render_zero_silence_region(
    AudioBuffer *audio,
    const TSM5Region *region,
    uint32_t sample_rate
) {
    uint64_t region_start_sample =
        absolute_tick_to_sample(
            region->start_ticks,
            region->time_unit_ns,
            sample_rate
        );

    uint64_t region_end_sample =
        absolute_tick_to_sample(
            region->start_ticks + region->duration_ticks,
            region->time_unit_ns,
            sample_rate
        );

    render_until_absolute_sample(audio, region_start_sample, 0);
    render_until_absolute_sample(audio, region_end_sample, 0);
}

static uint64_t read_raw_delta_from_payload(
    const uint8_t *payload,
    uint64_t *offset,
    uint32_t raw_delta_bytes
) {
    uint64_t value = 0;
    uint32_t i;

    for (i = 0; i < raw_delta_bytes; ++i) {
        value |= ((uint64_t)payload[*offset + i]) << (i * 8);
    }

    *offset += raw_delta_bytes;

    return value;
}

static uint8_t read_nibble_stream_code(
    const uint8_t *payload,
    uint64_t *nibble_pos
) {
    uint8_t byte = payload[*nibble_pos >> 1];
    uint8_t code;

    if ((*nibble_pos & 1) == 0) {
        code = byte & 0x0f;
    } else {
        code = (byte >> 4) & 0x0f;
    }

    (*nibble_pos)++;

    return code;
}

static void render_indexed_delta_region(
    AudioBuffer *audio,
    FILE *file,
    const TSM5Region *region,
    uint32_t sample_rate,
    int16_t amplitude,
    int *state
) {
    uint8_t *payload;
    uint32_t *table;

    uint64_t elapsed_ticks = 0;
    uint64_t region_end_ticks;

    if (
        region->encoding != TSM5_ENCODING_NIBBLE_PACKED_INDEX &&
        region->encoding != TSM5_ENCODING_INDEXED_ESCAPE_BYTE &&
        region->encoding != TSM5_ENCODING_NIBBLE_ESCAPE_INDEX
    ) {
        fprintf(stderr, "Unsupported indexed region encoding\n");
        exit(1);
    }

    payload = read_payload(file, region->data_offset, region->data_size);
    table = read_delta_table(file, region);

    if (region->reserved0 == 0 || region->reserved0 == 1) {
        *state = region->reserved0 ? 1 : -1;
    }

    render_region_gap_as_hold(
        audio,
        region,
        sample_rate,
        amplitude,
        *state
    );

    *state = -*state;

    region_end_ticks = region->start_ticks + region->duration_ticks;

    if (region->encoding == TSM5_ENCODING_NIBBLE_PACKED_INDEX) {
        uint64_t index_count = region->data_size * 2;
        uint64_t i;

        for (i = 0; i < index_count; ++i) {
            uint8_t index = read_nibble_index(payload, i);
            uint64_t delta_ticks;
            uint64_t target_ticks;
            uint64_t target_sample;

            if (index >= region->table_count) {
                index = 0;
            }

            delta_ticks = table[index];

            if (delta_ticks == 0) {
                continue;
            }

            elapsed_ticks += delta_ticks;
            target_ticks = region->start_ticks + elapsed_ticks;

            if (target_ticks > region_end_ticks) {
                target_ticks = region_end_ticks;
            }

            target_sample =
                absolute_tick_to_sample(
                    target_ticks,
                    region->time_unit_ns,
                    sample_rate
                );

            render_hold_until(audio, target_sample, amplitude, *state);
            *state = -*state;

            if (target_ticks >= region_end_ticks) {
                break;
            }
        }

    } else if (region->encoding == TSM5_ENCODING_INDEXED_ESCAPE_BYTE) {
        uint64_t offset = 0;

        while (offset < region->data_size) {
            uint8_t code = payload[offset++];
            uint64_t delta_ticks;
            uint64_t target_ticks;
            uint64_t target_sample;

            if (code == TSM5_ESCAPE_INDEX_BYTE) {
                if (offset + 4 > region->data_size) {
                    fprintf(stderr, "Malformed byte escape delta payload\n");
                    exit(1);
                }

                delta_ticks = read_u32_le_from_memory(payload, offset);
                offset += 4;

            } else {
                if (code >= region->table_count) {
                    code = 0;
                }

                delta_ticks = table[code];
            }

            if (delta_ticks == 0) {
                continue;
            }

            elapsed_ticks += delta_ticks;
            target_ticks = region->start_ticks + elapsed_ticks;

            if (target_ticks > region_end_ticks) {
                target_ticks = region_end_ticks;
            }

            target_sample =
                absolute_tick_to_sample(
                    target_ticks,
                    region->time_unit_ns,
                    sample_rate
                );

            render_hold_until(audio, target_sample, amplitude, *state);
            *state = -*state;

            if (target_ticks >= region_end_ticks) {
                break;
            }
        }

    } else {
        uint64_t nibble_pos = 0;
        uint64_t total_nibbles = region->data_size * 2;
        uint32_t raw_delta_bytes = region->flags;

        if (raw_delta_bytes < 1 || raw_delta_bytes > 4) {
            raw_delta_bytes = 4;
        }

        while (nibble_pos < total_nibbles) {
            uint8_t code = read_nibble_stream_code(payload, &nibble_pos);
            uint64_t delta_ticks;
            uint64_t target_ticks;
            uint64_t target_sample;

            if (code == TSM5_NIBBLE_ESCAPE_CODE) {
                uint64_t byte_offset;

                /*
                    Escape raw delta begins at the next byte boundary.
                */
                if (nibble_pos & 1) {
                    nibble_pos++;
                }

                byte_offset = nibble_pos >> 1;

                if (byte_offset + raw_delta_bytes > region->data_size) {
                    break;
                }

                delta_ticks =
                    read_raw_delta_from_payload(
                        payload,
                        &byte_offset,
                        raw_delta_bytes
                    );

                nibble_pos = byte_offset * 2;

            } else {
                if (code >= region->table_count) {
                    code = 0;
                }

                delta_ticks = table[code];
            }

            if (delta_ticks == 0) {
                continue;
            }

            elapsed_ticks += delta_ticks;
            target_ticks = region->start_ticks + elapsed_ticks;

            if (target_ticks > region_end_ticks) {
                target_ticks = region_end_ticks;
            }

            target_sample =
                absolute_tick_to_sample(
                    target_ticks,
                    region->time_unit_ns,
                    sample_rate
                );

            render_hold_until(audio, target_sample, amplitude, *state);
            *state = -*state;

            if (target_ticks >= region_end_ticks) {
                break;
            }
        }
    }

    {
        uint64_t region_end_sample =
            absolute_tick_to_sample(
                region_end_ticks,
                region->time_unit_ns,
                sample_rate
            );

        render_hold_until(audio, region_end_sample, amplitude, *state);
    }

    free(payload);
    free(table);
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path;

    uint32_t sample_rate = 96000;
    int amplitude = 22000;

    FILE *file;
    TSM5Header header;
    TSM5Region *regions;

    AudioBuffer audio;
    int state;
    uint32_t i;

    uint64_t expected_total_samples;

    if (argc < 3) {
        printf("Usage:\n");
        printf("  %s input.tsm output.wav [sample_rate] [amplitude]\n", argv[0]);
        return 0;
    }

    input_path = argv[1];
    output_path = argv[2];

    if (argc >= 4) {
        sample_rate = (uint32_t)strtoul(argv[3], NULL, 0);
    }

    if (argc >= 5) {
        amplitude = atoi(argv[4]);
    }

    if (sample_rate < 8000) {
        fprintf(stderr, "ERROR: sample_rate is too low\n");
        return 1;
    }

    if (amplitude < 1 || amplitude > 32767) {
        fprintf(stderr, "ERROR: amplitude must be between 1 and 32767\n");
        return 1;
    }

    file = fopen(input_path, "rb");

    if (!file) {
        fprintf(stderr, "Cannot open TSM: %s\n", input_path);
        return 1;
    }

    if (!tsm5_read_header(file, &header)) {
        fprintf(stderr, "Invalid TSM v5 file\n");
        fclose(file);
        return 1;
    }

    regions =
        (TSM5Region *)calloc(
            header.region_count,
            sizeof(TSM5Region)
        );

    if (!regions) {
        fprintf(stderr, "Out of memory while reading region table\n");
        fclose(file);
        return 1;
    }

    for (i = 0; i < header.region_count; ++i) {
        fseek(
            file,
            (long)(header.region_table_offset + (uint64_t)i * header.region_entry_size),
            SEEK_SET
        );

        if (!tsm5_read_region(file, &regions[i])) {
            fprintf(stderr, "Cannot read region %u\n", i);
            fclose(file);
            free(regions);
            return 1;
        }
    }

    audio_init(&audio);
    state = header.reserved0 ? 1 : -1;

    for (i = 0; i < header.region_count; ++i) {
        if (regions[i].region_mode == TSM5_MODE_SILENCE) {
            render_silence_region(
                &audio,
                &regions[i],
                sample_rate,
                (int16_t)amplitude,
                &state
            );

        } else if (regions[i].region_mode == TSM5_MODE_ZERO_SILENCE) {
            render_zero_silence_region(
                &audio,
                &regions[i],
                sample_rate
            );

        } else if (regions[i].region_mode == TSM5_MODE_INDEXED_DELTA) {
            render_indexed_delta_region(
                &audio,
                file,
                &regions[i],
                sample_rate,
                (int16_t)amplitude,
                &state
            );

        } else {
            fprintf(
                stderr,
                "Skipping unsupported region mode %u\n",
                regions[i].region_mode
            );
        }
    }

    expected_total_samples =
        expected_output_samples_from_header(
            &header,
            sample_rate
        );

    render_hold_until(
        &audio,
        expected_total_samples,
        (int16_t)amplitude,
        state
    );

    audio_trim(&audio, expected_total_samples);

    fclose(file);

    if (!wav_write_pcm16_mono_simple(
            output_path,
            audio.samples,
            audio.count,
            sample_rate
        )) {
        audio_free(&audio);
        free(regions);
        return 1;
    }

    printf("TSM v5.2 to WAV completed\n");
    printf("Input: %s\n", input_path);
    printf("Output: %s\n", output_path);
    printf("Regions: %u\n", header.region_count);
    printf("Initial signal state: %s\n", header.reserved0 ? "HIGH" : "LOW");

    printf("Samples: ");
    print_u64(audio.count);
    printf("\n");

    printf("Expected samples: ");
    print_u64(expected_total_samples);
    printf("\n");

    if (header.reserved1 != 0 && header.reserved2 != 0) {
        printf("Source duration metadata: ");
        print_u64(header.reserved1);
        printf(" samples @ ");
        print_u64(header.reserved2);
        printf(" Hz\n");
    }

    printf("Renderer: absolute timing + nibble escape + zero silence\n");

    audio_free(&audio);
    free(regions);

    return 0;
}
