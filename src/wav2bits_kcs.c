/*
 * TSM - Temporal Signal Medium
 * WAV to a text bit stream, for looking at a KCS tape by eye.
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
#include "indexed_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    wav2bits_kcs.c

    WAV -> text bit stream diagnostic utility.

    Purpose
    -------

    This tool converts a KCS-like waveform into a simple text stream:

        '0' = one full 1200 Hz wave
              approximately 833.33 us total
              represented by 2 long half-periods

        '1' = one full 2400 Hz wave
              approximately 416.67 us total
              represented by 2 short half-periods

        ' ' = approximately 833.33 us of zero-level silence

    Important
    ---------

    This is a diagnostic/repair-inspection tool, not an archival tool.

    It is useful after cleaning a WAV with framed KCS repair, because it lets
    you visually inspect whether the repaired signal obeys the expected
    high/low wave grammar.

    Defaults with time_unit_ns = 4000:

        short half-period = 52 ticks  = 208 us
        long  half-period = 104 ticks = 416 us
        silence unit      = 208 ticks = 832 us

    Usage
    -----

        wav2bits_kcs.exe input.wav output.txt 4000 0.35 1 8.0 1.0 52 104 208 0.35

    Arguments
    ---------

        input.wav
        output.txt
        time_unit_ns
        edge_threshold
        min_delta_ticks
        gain
        clip_level
        short_ticks
        long_ticks
        silence_ticks
        max_relative_error

    Output
    ------

        The output file contains a text stream made of:

            0
            1
            space

        plus line breaks every 128 characters for readability.
*/

typedef struct {
    char *data;
    uint64_t count;
    uint64_t capacity;
} CharBuffer;

static void print_u64(uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    printf("%s", text);
}

static void char_buffer_init(CharBuffer *buffer) {
    buffer->data = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static void char_buffer_free(CharBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void char_buffer_push(CharBuffer *buffer, char value) {
    if (buffer->count + 1 > buffer->capacity) {
        uint64_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 4096;

        char *new_data =
            (char *)realloc(
                buffer->data,
                (size_t)new_capacity
            );

        if (!new_data) {
            fprintf(stderr, "Out of memory while growing text buffer\n");
            exit(1);
        }

        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }

    buffer->data[buffer->count++] = value;
}

static uint64_t abs_diff_u64_local(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

static double relative_error(uint64_t measured, uint64_t target) {
    if (target == 0) {
        return 1.0e30;
    }

    return (double)abs_diff_u64_local(measured, target) / (double)target;
}

static double symbol_error(
    const U64List *deltas,
    uint64_t offset,
    uint32_t count,
    uint64_t target
) {
    long double error_sum = 0.0L;
    uint32_t i;

    if (offset + count > deltas->count) {
        return 1.0e30;
    }

    for (i = 0; i < count; ++i) {
        error_sum +=
            (long double)relative_error(
                deltas->items[offset + i],
                target
            );
    }

    return (double)(error_sum / (long double)count);
}

static int write_text_stream(
    const char *path,
    const CharBuffer *buffer,
    uint32_t line_width
) {
    FILE *file = fopen(path, "wb");
    uint64_t i;

    if (!file) {
        fprintf(stderr, "Cannot write output text file: %s\n", path);
        return 0;
    }

    if (line_width == 0) {
        line_width = 128;
    }

    for (i = 0; i < buffer->count; ++i) {
        fputc(buffer->data[i], file);

        if (((i + 1) % line_width) == 0) {
            fputc('\n', file);
        }
    }

    if ((buffer->count % line_width) != 0) {
        fputc('\n', file);
    }

    fclose(file);

    return 1;
}

static void append_silence_chars_for_ticks(
    CharBuffer *out,
    uint64_t ticks,
    uint64_t silence_ticks
) {
    uint64_t count;
    uint64_t i;

    if (silence_ticks == 0) {
        return;
    }

    count = (ticks + (silence_ticks / 2)) / silence_ticks;

    if (count == 0 && ticks > 0) {
        count = 1;
    }

    for (i = 0; i < count; ++i) {
        char_buffer_push(out, ' ');
    }
}

static void decode_deltas_to_bits(
    const U64List *deltas,
    CharBuffer *out,
    uint64_t short_ticks,
    uint64_t long_ticks,
    uint64_t silence_ticks,
    double max_relative_error,
    uint64_t *out_zero_count,
    uint64_t *out_one_count,
    uint64_t *out_space_count,
    uint64_t *out_unknown_count
) {
    uint64_t i = 0;
    uint64_t zeros = 0;
    uint64_t ones = 0;
    uint64_t spaces = 0;
    uint64_t unknown = 0;

    while (i < deltas->count) {
        uint64_t d = deltas->items[i];

        /*
            A silence unit is 833.33 us, which is one full 1200 Hz wave time.
            Long real gaps become one or more spaces.
        */
        if (
            silence_ticks > 0 &&
            d >= (silence_ticks - (silence_ticks / 4))
        ) {
            uint64_t before = out->count;

            append_silence_chars_for_ticks(out, d, silence_ticks);
            spaces += out->count - before;
            i++;
            continue;
        }

        {
            /*
                '0' means one full 1200 Hz wave:
                    long + long

                '1' means one full 2400 Hz wave:
                    short + short

                This intentionally differs from the framed repair bit grammar
                where a data bit 1 may be represented by four short half-waves.
                Here we output one char per full carrier wave.
            */
            double zero_cost = symbol_error(deltas, i, 2, long_ticks);
            double one_cost = symbol_error(deltas, i, 2, short_ticks);

            if (zero_cost <= one_cost && zero_cost <= max_relative_error) {
                char_buffer_push(out, '0');
                zeros++;
                i += 2;

            } else if (one_cost < zero_cost && one_cost <= max_relative_error) {
                char_buffer_push(out, '1');
                ones++;
                i += 2;

            } else {
                /*
                    Unknown or damaged local section.  Use '?' so the user can
                    see where the parser lost confidence.
                */
                char_buffer_push(out, '?');
                unknown++;
                i++;
            }
        }
    }

    *out_zero_count = zeros;
    *out_one_count = ones;
    *out_space_count = spaces;
    *out_unknown_count = unknown;
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path;

    uint64_t time_unit_ns;
    double edge_threshold;
    uint64_t min_delta_ticks;
    double gain;
    double clip_level;
    uint64_t short_ticks;
    uint64_t long_ticks;
    uint64_t silence_ticks;
    double max_relative_error;

    WavData wav;
    ConditioningStats conditioning;

    U64List edges;
    U64List deltas;

    CharBuffer text;

    uint64_t zeros = 0;
    uint64_t ones = 0;
    uint64_t spaces = 0;
    uint64_t unknown = 0;

    uint64_t first_edge_ticks = 0;

    if (argc < 12) {
        printf("Usage:\n");
        printf("  %s input.wav output.txt time_unit_ns edge_threshold min_delta_ticks gain clip_level short_ticks long_ticks silence_ticks max_relative_error\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s input.wav bits.txt 4000 0.35 1 8.0 1.0 52 104 208 0.35\n", argv[0]);
        printf("\nMeaning:\n");
        printf("  '0' = one 1200 Hz wave = two long half-periods\n");
        printf("  '1' = one 2400 Hz wave = two short half-periods\n");
        printf("  ' ' = approximately one 833.33 us silence unit\n");
        return 0;
    }

    input_path = argv[1];
    output_path = argv[2];

    time_unit_ns = (uint64_t)strtoull(argv[3], NULL, 0);
    edge_threshold = atof(argv[4]);
    min_delta_ticks = (uint64_t)strtoull(argv[5], NULL, 0);
    gain = atof(argv[6]);
    clip_level = atof(argv[7]);
    short_ticks = (uint64_t)strtoull(argv[8], NULL, 0);
    long_ticks = (uint64_t)strtoull(argv[9], NULL, 0);
    silence_ticks = (uint64_t)strtoull(argv[10], NULL, 0);
    max_relative_error = atof(argv[11]);

    if (!wav_load_pcm16_mono_simple(input_path, &wav)) {
        return 1;
    }

    condition_signal(
        &wav,
        gain,
        clip_level,
        &conditioning
    );

    u64_list_init(&edges);
    u64_list_init(&deltas);

    detect_edges_schmitt(
        &wav,
        time_unit_ns,
        edge_threshold,
        min_delta_ticks,
        &edges
    );

    build_deltas(&edges, &deltas);

    char_buffer_init(&text);

    if (edges.count > 0) {
        /*
            Convert leading silence before the first detected edge into spaces.
        */
        first_edge_ticks = edges.items[0];

        if (first_edge_ticks >= silence_ticks / 2) {
            append_silence_chars_for_ticks(
                &text,
                first_edge_ticks,
                silence_ticks
            );
            spaces = text.count;
        }
    }

    decode_deltas_to_bits(
        &deltas,
        &text,
        short_ticks,
        long_ticks,
        silence_ticks,
        max_relative_error,
        &zeros,
        &ones,
        &spaces,
        &unknown
    );

    if (!write_text_stream(output_path, &text, 128)) {
        char_buffer_free(&text);
        u64_list_free(&edges);
        u64_list_free(&deltas);
        wav_free_simple(&wav);
        return 1;
    }

    printf("WAV to KCS bit text completed\n");
    printf("Input: %s\n", input_path);
    printf("Output: %s\n", output_path);
    printf("Sample rate: %u Hz\n", wav.sample_rate);
    printf("Edges: ");
    print_u64(edges.count);
    printf("\n");
    printf("Deltas: ");
    print_u64(deltas.count);
    printf("\n");
    printf("0 chars: ");
    print_u64(zeros);
    printf("\n");
    printf("1 chars: ");
    print_u64(ones);
    printf("\n");
    printf("space chars: ");
    print_u64(spaces);
    printf("\n");
    printf("unknown chars: ");
    print_u64(unknown);
    printf("\n");
    printf("Output text chars: ");
    print_u64(text.count);
    printf("\n");

    char_buffer_free(&text);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
