#include "tsm_v5.h"
#include "wav_io_simple.h"
#include "indexed_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    wav_repair_kcs.c

    WAV -> repaired WAV utility.

    This is not an archival converter. It is a guided repair tool.

    It detects edges in a conditioned WAV, measures deltas, then snaps noisy
    deltas to a known/canonical timing table.

    Profiles:
        kcs_strict
            52, 104

        kcs_adaptive
            estimates the short and long timing families from the WAV

        kcs_extended
            estimates short, middle, long, and extra timing families

        custom:52,104
        custom:52,78,104,109

    Recommended flow:
        dirty.wav
        -> wav_repair_kcs.exe
        -> clean_repaired.wav
        -> wav2tsm_indexed_v5.exe
        -> TSM v5.2
*/

#define MAX_REPAIR_TABLE 15

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

static void audio_extend_or_trim(AudioBuffer *audio, uint64_t target_count, int16_t value) {
    if (audio->count < target_count) {
        audio_append(audio, value, target_count - audio->count);
    } else if (audio->count > target_count) {
        audio->count = target_count;
    }
}

static uint64_t abs_diff_u64_local(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

static int nearest_table_index_local(
    uint64_t delta,
    const uint64_t *table,
    uint32_t table_count
) {
    uint32_t best = 0;
    uint64_t best_error = UINT64_MAX;
    uint32_t i;

    for (i = 0; i < table_count; ++i) {
        uint64_t error = abs_diff_u64_local(delta, table[i]);

        if (error < best_error) {
            best_error = error;
            best = i;
        }
    }

    return (int)best;
}

static void sort_table(uint64_t *table, uint32_t table_count) {
    uint32_t i;
    uint32_t j;

    for (i = 0; i < table_count; ++i) {
        for (j = i + 1; j < table_count; ++j) {
            if (table[j] < table[i]) {
                uint64_t t = table[i];
                table[i] = table[j];
                table[j] = t;
            }
        }
    }
}

static int parse_custom_table(
    const char *profile,
    uint64_t *table,
    uint32_t *table_count
) {
    const char *p;
    uint32_t count = 0;

    if (strncmp(profile, "custom:", 7) != 0) {
        return 0;
    }

    p = profile + 7;

    while (*p && count < MAX_REPAIR_TABLE) {
        char *end_ptr;
        unsigned long value;

        while (*p == ' ' || *p == ',') {
            p++;
        }

        if (!*p) {
            break;
        }

        value = strtoul(p, &end_ptr, 10);

        if (end_ptr == p) {
            break;
        }

        table[count++] = (uint64_t)value;
        p = end_ptr;
    }

    *table_count = count;
    sort_table(table, *table_count);

    return count > 0;
}

static uint64_t weighted_average_band(
    const DeltaRank *ranks,
    uint64_t rank_count,
    uint64_t min_delta,
    uint64_t max_delta,
    uint64_t fallback
) {
    uint64_t i;
    long double weighted_sum = 0.0L;
    long double count_sum = 0.0L;

    for (i = 0; i < rank_count; ++i) {
        uint64_t d = ranks[i].delta;

        if (d >= min_delta && d <= max_delta) {
            weighted_sum += (long double)d * (long double)ranks[i].count;
            count_sum += (long double)ranks[i].count;
        }
    }

    if (count_sum <= 0.0L) {
        return fallback;
    }

    return (uint64_t)((weighted_sum / count_sum) + 0.5L);
}

static void build_repair_table(
    const char *profile,
    const DeltaRank *ranks,
    uint64_t rank_count,
    uint64_t *table,
    uint32_t *table_count
) {
    if (parse_custom_table(profile, table, table_count)) {
        return;
    }

    if (strcmp(profile, "kcs_strict") == 0) {
        table[0] = 52;
        table[1] = 104;
        *table_count = 2;
        return;
    }

    if (strcmp(profile, "kcs_adaptive") == 0) {
        table[0] = weighted_average_band(ranks, rank_count, 40, 70, 52);
        table[1] = weighted_average_band(ranks, rank_count, 95, 130, 104);
        *table_count = 2;
        sort_table(table, *table_count);
        return;
    }

    if (strcmp(profile, "kcs_extended") == 0) {
        table[0] = weighted_average_band(ranks, rank_count, 40, 70, 52);
        table[1] = weighted_average_band(ranks, rank_count, 71, 95, 78);
        table[2] = weighted_average_band(ranks, rank_count, 96, 130, 104);
        table[3] = weighted_average_band(ranks, rank_count, 131, 170, 153);
        *table_count = 4;
        sort_table(table, *table_count);
        return;
    }

    fprintf(stderr, "ERROR: unknown repair profile: %s\n", profile);
    fprintf(stderr, "Use kcs_strict, kcs_adaptive, kcs_extended, or custom:52,104\n");
    exit(1);
}

static void render_repaired_wave(
    const WavData *wav,
    const U64List *edges,
    const U64List *deltas,
    const uint64_t *repair_table,
    uint32_t repair_table_count,
    uint64_t time_unit_ns,
    uint64_t raw_escape_threshold_ticks,
    int16_t amplitude,
    int initial_high,
    AudioBuffer *audio,
    uint64_t *out_repaired,
    uint64_t *out_preserved_raw
) {
    uint64_t i;
    int state = initial_high ? 1 : -1;
    uint64_t current_ticks = 0;
    uint64_t target_source_samples = wav->sample_count;

    *out_repaired = 0;
    *out_preserved_raw = 0;

    audio_init(audio);

    /*
        Preserve the time before the first detected edge as zero.
    */
    if (edges->count > 0 && edges->items[0] > 0) {
        uint64_t first_sample =
            tsm5_ticks_to_samples(
                edges->items[0],
                time_unit_ns,
                wav->sample_rate
            );

        audio_append(audio, 0, first_sample);
        current_ticks = edges->items[0];
    }

    /*
        The first edge switches from the initial state.
    */
    state = -state;

    for (i = 0; i < deltas->count; ++i) {
        uint64_t source_delta = deltas->items[i];
        uint64_t output_delta;
        uint64_t target_ticks;
        uint64_t target_sample;

        if (
            raw_escape_threshold_ticks > 0 &&
            source_delta >= raw_escape_threshold_ticks
        ) {
            /*
                Preserve very large gaps as zero signal.  This is repair-safe
                because such intervals are not normal KCS pulses.
            */
            output_delta = source_delta;
            target_ticks = current_ticks + output_delta;
            target_sample =
                tsm5_ticks_to_samples(
                    target_ticks,
                    time_unit_ns,
                    wav->sample_rate
                );

            if (target_sample > audio->count) {
                audio_append(audio, 0, target_sample - audio->count);
            }

            (*out_preserved_raw)++;

        } else {
            int idx =
                nearest_table_index_local(
                    source_delta,
                    repair_table,
                    repair_table_count
                );

            output_delta = repair_table[idx];
            target_ticks = current_ticks + output_delta;
            target_sample =
                tsm5_ticks_to_samples(
                    target_ticks,
                    time_unit_ns,
                    wav->sample_rate
                );

            if (target_sample > audio->count) {
                audio_append(
                    audio,
                    state > 0 ? amplitude : (int16_t)-amplitude,
                    target_sample - audio->count
                );
            }

            (*out_repaired)++;
        }

        current_ticks = target_ticks;
        state = -state;
    }

    audio_extend_or_trim(audio, target_source_samples, 0);
}

int main(int argc, char **argv) {
    const char *input_wav;
    const char *output_wav;
    const char *profile;

    uint64_t time_unit_ns;
    double edge_threshold;
    uint64_t min_delta_ticks;
    double gain;
    double clip_level;
    uint64_t raw_escape_threshold_ticks;
    int amplitude;

    WavData wav;
    ConditioningStats stats;
    int initial_high;

    U64List edges;
    U64List deltas;
    DeltaRank *ranks;
    uint64_t rank_count = 0;

    uint64_t repair_table[MAX_REPAIR_TABLE];
    uint32_t repair_table_count = 0;

    AudioBuffer audio;
    uint64_t repaired = 0;
    uint64_t preserved_raw = 0;
    uint32_t i;

    if (argc < 9) {
        printf("Usage:\n");
        printf("  %s input.wav output.wav profile time_unit_ns edge_threshold min_delta_ticks gain [clip_level] [raw_escape_threshold_ticks] [amplitude]\n", argv[0]);
        printf("\nProfiles:\n");
        printf("  kcs_strict\n");
        printf("  kcs_adaptive\n");
        printf("  kcs_extended\n");
        printf("  custom:52,104\n");
        printf("\nExample:\n");
        printf("  %s dirty.wav clean.wav kcs_strict 4000 0.35 1 8.0 1.0 5000 22000\n", argv[0]);
        return 0;
    }

    input_wav = argv[1];
    output_wav = argv[2];
    profile = argv[3];

    time_unit_ns = (uint64_t)strtoull(argv[4], NULL, 0);
    edge_threshold = atof(argv[5]);
    min_delta_ticks = (uint64_t)strtoull(argv[6], NULL, 0);
    gain = atof(argv[7]);
    clip_level = argc >= 9 ? atof(argv[8]) : 1.0;
    raw_escape_threshold_ticks = argc >= 10 ? (uint64_t)strtoull(argv[9], NULL, 0) : 5000;
    amplitude = argc >= 11 ? atoi(argv[10]) : 22000;

    if (!wav_load_pcm16_mono_simple(input_wav, &wav)) {
        return 1;
    }

    condition_signal(&wav, gain, clip_level, &stats);

    initial_high =
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

    ranks = make_delta_ranking(&deltas, &rank_count);

    build_repair_table(
        profile,
        ranks,
        rank_count,
        repair_table,
        &repair_table_count
    );

    render_repaired_wave(
        &wav,
        &edges,
        &deltas,
        repair_table,
        repair_table_count,
        time_unit_ns,
        raw_escape_threshold_ticks,
        (int16_t)amplitude,
        initial_high,
        &audio,
        &repaired,
        &preserved_raw
    );

    if (!wav_write_pcm16_mono_simple(output_wav, audio.samples, audio.count, wav.sample_rate)) {
        audio_free(&audio);
        free(ranks);
        u64_list_free(&edges);
        u64_list_free(&deltas);
        wav_free_simple(&wav);
        return 1;
    }

    printf("WAV repair completed\n");
    printf("Input: %s\n", input_wav);
    printf("Output: %s\n", output_wav);
    printf("Profile: %s\n", profile);
    printf("Repair table:");

    for (i = 0; i < repair_table_count; ++i) {
        printf(" ");
        print_u64(repair_table[i]);
    }

    printf("\n");
    printf("Edges: ");
    print_u64(edges.count);
    printf("\n");
    printf("Deltas: ");
    print_u64(deltas.count);
    printf("\n");
    printf("Repaired deltas: ");
    print_u64(repaired);
    printf("\n");
    printf("Preserved large gaps: ");
    print_u64(preserved_raw);
    printf("\n");
    printf("Samples: ");
    print_u64(audio.count);
    printf("\n");
    printf("Sample rate: %u Hz\n", wav.sample_rate);

    audio_free(&audio);
    free(ranks);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
