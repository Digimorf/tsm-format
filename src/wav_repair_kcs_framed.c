/*
 * TSM - Temporal Signal Medium
 * Phase-locked repair of a framed KCS waveform.
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
    wav_repair_kcs_framed.c

    Phase-locked KCS-style framed waveform repair utility.

    This is a REPAIR tool, not an archival tool.

    Difference from the previous framed repair
    ------------------------------------------

    The previous version respected the KCS duration grammar:

        bit 0 = 2 half-periods at 1200 Hz
        bit 1 = 4 half-periods at 2400 Hz

    but the symbol could start either HIGH or LOW depending on the current
    carried waveform state.

    This version can force every reconstructed bit to start with a HIGH
    half-period:

        bit 0 = HIGH long,  LOW long
        bit 1 = HIGH short, LOW short, HIGH short, LOW short

    This is useful when the target loader expects each data bit/symbol to have
    a consistent phase.

    Usage
    -----

        wav_repair_kcs_framed.exe dirty.wav repaired.wav 4000 0.35 1 8.0 1.0 52 104 5000 0.35 1

    Last argument:

        force_symbol_high_start
            0 = keep continuous carried phase
            1 = force every repaired bit to start HIGH

    Suggested default for repair:
        force_symbol_high_start = 1
*/

typedef struct {
    int16_t *samples;
    uint64_t count;
    uint64_t capacity;
} AudioBuffer;

typedef struct {
    uint64_t zeros;
    uint64_t ones;
    uint64_t fallback_single;
    uint64_t preserved_gaps;
    uint64_t forced_high_starts;
} RepairStats;

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

static void audio_append(AudioBuffer *audio, int16_t value, uint64_t count) {
    uint64_t i;

    if (count == 0) {
        return;
    }

    audio_reserve(audio, audio->count + count);

    for (i = 0; i < count; ++i) {
        audio->samples[audio->count++] = value;
    }
}

static uint64_t ticks_to_samples_local(
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
) {
    return tsm5_ticks_to_samples(ticks, time_unit_ns, sample_rate);
}

static void append_ticks(
    AudioBuffer *audio,
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int state,
    int16_t amplitude
) {
    uint64_t sample_count =
        ticks_to_samples_local(
            ticks,
            time_unit_ns,
            sample_rate
        );

    audio_append(
        audio,
        state > 0 ? amplitude : (int16_t)-amplitude,
        sample_count
    );
}

static void append_zero_ticks(
    AudioBuffer *audio,
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
) {
    uint64_t sample_count =
        ticks_to_samples_local(
            ticks,
            time_unit_ns,
            sample_rate
        );

    audio_append(audio, 0, sample_count);
}

static uint64_t delta_error_abs(uint64_t measured, uint64_t target) {
    return measured > target ? measured - target : target - measured;
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
        uint64_t measured = deltas->items[offset + i];
        uint64_t error = delta_error_abs(measured, target);

        error_sum += (long double)error / (long double)target;
    }

    return (double)(error_sum / (long double)count);
}

static void force_symbol_start_high_if_needed(
    int *state,
    int force_symbol_high_start,
    RepairStats *stats
) {
    if (force_symbol_high_start && *state < 0) {
        *state = 1;
        stats->forced_high_starts++;
    }
}

static void append_kcs_zero(
    AudioBuffer *audio,
    uint64_t long_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int *state,
    int16_t amplitude,
    int force_symbol_high_start,
    RepairStats *stats
) {
    force_symbol_start_high_if_needed(
        state,
        force_symbol_high_start,
        stats
    );

    append_ticks(audio, long_ticks, time_unit_ns, sample_rate, *state, amplitude);
    *state = -*state;

    append_ticks(audio, long_ticks, time_unit_ns, sample_rate, *state, amplitude);
    *state = -*state;
}

static void append_kcs_one(
    AudioBuffer *audio,
    uint64_t short_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int *state,
    int16_t amplitude,
    int force_symbol_high_start,
    RepairStats *stats
) {
    int i;

    force_symbol_start_high_if_needed(
        state,
        force_symbol_high_start,
        stats
    );

    for (i = 0; i < 4; ++i) {
        append_ticks(audio, short_ticks, time_unit_ns, sample_rate, *state, amplitude);
        *state = -*state;
    }
}

static void repair_render_deltas(
    const U64List *deltas,
    AudioBuffer *audio,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    uint64_t short_ticks,
    uint64_t long_ticks,
    uint64_t gap_threshold_ticks,
    double max_relative_error,
    int *state,
    int16_t amplitude,
    int force_symbol_high_start,
    RepairStats *stats
) {
    uint64_t i = 0;

    memset(stats, 0, sizeof(*stats));

    while (i < deltas->count) {
        uint64_t d = deltas->items[i];

        /*
            Large intervals are not data half-periods. Preserve as zero-level
            silence. After a real gap, restart the next symbol HIGH if requested.
        */
        if (gap_threshold_ticks > 0 && d >= gap_threshold_ticks) {
            append_zero_ticks(audio, d, time_unit_ns, sample_rate);
            stats->preserved_gaps++;

            if (force_symbol_high_start) {
                *state = 1;
            }

            i++;
            continue;
        }

        {
            double zero_cost = symbol_error(deltas, i, 2, long_ticks);
            double one_cost = symbol_error(deltas, i, 4, short_ticks);

            if (zero_cost <= one_cost && zero_cost <= max_relative_error) {
                append_kcs_zero(
                    audio,
                    long_ticks,
                    time_unit_ns,
                    sample_rate,
                    state,
                    amplitude,
                    force_symbol_high_start,
                    stats
                );

                stats->zeros++;
                i += 2;

            } else if (one_cost < zero_cost && one_cost <= max_relative_error) {
                append_kcs_one(
                    audio,
                    short_ticks,
                    time_unit_ns,
                    sample_rate,
                    state,
                    amplitude,
                    force_symbol_high_start,
                    stats
                );

                stats->ones++;
                i += 4;

            } else {
                /*
                    Fallback: if we cannot recognize a valid bit, canonicalize
                    one half-period.  In high-start mode, force this fallback
                    pulse HIGH too, because it is likely at a damaged boundary.
                */
                uint64_t short_error = delta_error_abs(d, short_ticks);
                uint64_t long_error = delta_error_abs(d, long_ticks);
                uint64_t target = short_error <= long_error ? short_ticks : long_ticks;

                if (force_symbol_high_start && *state < 0) {
                    *state = 1;
                    stats->forced_high_starts++;
                }

                append_ticks(
                    audio,
                    target,
                    time_unit_ns,
                    sample_rate,
                    *state,
                    amplitude
                );

                *state = -*state;
                stats->fallback_single++;
                i++;
            }
        }
    }
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
    uint64_t gap_threshold_ticks;
    double max_relative_error;
    int force_symbol_high_start = 1;

    WavData wav;
    ConditioningStats conditioning;

    U64List edges;
    U64List deltas;

    AudioBuffer audio;
    RepairStats stats;

    int initial_state_high;
    int state;
    int16_t amplitude = 22000;

    uint64_t first_edge_ticks = 0;
    uint64_t total_expected_samples;

    if (argc < 12) {
        printf("Usage:\n");
        printf("  %s input.wav output.wav time_unit_ns edge_threshold min_delta_ticks gain clip_level short_ticks long_ticks gap_threshold_ticks max_relative_error [force_symbol_high_start]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s dirty.wav repaired.wav 4000 0.35 1 8.0 1.0 52 104 5000 0.35 1\n", argv[0]);
        printf("\nforce_symbol_high_start:\n");
        printf("  0 = keep continuous carried phase\n");
        printf("  1 = force each repaired bit to start HIGH\n");
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
    gap_threshold_ticks = (uint64_t)strtoull(argv[10], NULL, 0);
    max_relative_error = atof(argv[11]);

    if (argc >= 13) {
        force_symbol_high_start = atoi(argv[12]) ? 1 : 0;
    }

    if (!wav_load_pcm16_mono_simple(input_path, &wav)) {
        return 1;
    }

    condition_signal(
        &wav,
        gain,
        clip_level,
        &conditioning
    );

    initial_state_high =
        detect_initial_signal_state(
            &wav,
            edge_threshold
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

    audio_init(&audio);

    /*
        In phase-locked repair mode, the data symbol generator starts HIGH.
        We still keep the initial-state detection for reporting and for the
        continuous mode.
    */
    state =
        force_symbol_high_start
            ? 1
            : (initial_state_high ? 1 : -1);

    if (edges.count > 0) {
        first_edge_ticks = edges.items[0];

        append_zero_ticks(
            &audio,
            first_edge_ticks,
            time_unit_ns,
            wav.sample_rate
        );

        /*
            In continuous mode, the first detected edge toggles into the active
            signal.  In high-start mode, the first repaired symbol starts HIGH.
        */
        if (!force_symbol_high_start) {
            state = -state;
        } else {
            state = 1;
        }
    }

    repair_render_deltas(
        &deltas,
        &audio,
        time_unit_ns,
        wav.sample_rate,
        short_ticks,
        long_ticks,
        gap_threshold_ticks,
        max_relative_error,
        &state,
        amplitude,
        force_symbol_high_start,
        &stats
    );

    total_expected_samples = wav.sample_count;

    if (audio.count < total_expected_samples) {
        audio_append(
            &audio,
            0,
            total_expected_samples - audio.count
        );

    } else if (audio.count > total_expected_samples) {
        audio.count = total_expected_samples;
    }

    if (!wav_write_pcm16_mono_simple(output_path, audio.samples, audio.count, wav.sample_rate)) {
        audio_free(&audio);
        u64_list_free(&edges);
        u64_list_free(&deltas);
        wav_free_simple(&wav);
        return 1;
    }

    printf("KCS framed phase-locked repair completed\n");
    printf("Input: %s\n", input_path);
    printf("Output: %s\n", output_path);
    printf("Sample rate: %u Hz\n", wav.sample_rate);
    printf("Force symbol high start: %s\n", force_symbol_high_start ? "YES" : "NO");
    printf("Short ticks: ");
    print_u64(short_ticks);
    printf("\n");
    printf("Long ticks: ");
    print_u64(long_ticks);
    printf("\n");
    printf("Detected edges: ");
    print_u64(edges.count);
    printf("\n");
    printf("Detected deltas: ");
    print_u64(deltas.count);
    printf("\n");
    printf("Decoded KCS zeros: ");
    print_u64(stats.zeros);
    printf("\n");
    printf("Decoded KCS ones: ");
    print_u64(stats.ones);
    printf("\n");
    printf("Fallback single half-periods: ");
    print_u64(stats.fallback_single);
    printf("\n");
    printf("Preserved large gaps: ");
    print_u64(stats.preserved_gaps);
    printf("\n");
    printf("Forced HIGH starts: ");
    print_u64(stats.forced_high_starts);
    printf("\n");
    printf("Output samples: ");
    print_u64(audio.count);
    printf("\n");

    audio_free(&audio);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
