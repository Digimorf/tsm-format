/*
 * TSM - Temporal Signal Medium
 * Shared helpers for the indexed-delta encoding.
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

#include "indexed_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void u64_list_init(U64List *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void u64_list_free(U64List *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void u64_list_push(U64List *list, uint64_t value) {
    if (list->count + 1 > list->capacity) {
        uint64_t new_capacity = list->capacity ? list->capacity * 2 : 4096;

        uint64_t *new_items =
            (uint64_t *)realloc(
                list->items,
                (size_t)new_capacity * sizeof(uint64_t)
            );

        if (!new_items) {
            fprintf(stderr, "Out of memory while growing uint64 list\n");
            exit(1);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = value;
}

void condition_signal(
    WavData *wav,
    double gain,
    double clip_level,
    ConditioningStats *stats
) {
    double sum = 0.0;
    double dc;
    double peak = 0.0;
    uint64_t clipped = 0;
    uint64_t i;

    memset(stats, 0, sizeof(*stats));

    if (gain <= 0.0) {
        gain = 1.0;
    }

    if (clip_level <= 0.0) {
        clip_level = 1.0;
    }

    for (i = 0; i < wav->sample_count; ++i) {
        sum += wav->samples[i];
    }

    dc =
        wav->sample_count
            ? sum / (double)wav->sample_count
            : 0.0;

    for (i = 0; i < wav->sample_count; ++i) {
        double sample = wav->samples[i] - dc;
        double absolute = fabs(sample);

        if (absolute > peak) {
            peak = absolute;
        }

        sample *= gain;

        if (sample > clip_level) {
            sample = clip_level;
            clipped++;

        } else if (sample < -clip_level) {
            sample = -clip_level;
            clipped++;
        }

        wav->samples[i] = sample / clip_level;
    }

    stats->dc_offset = dc;
    stats->peak_before_gain = peak;
    stats->gain = gain;
    stats->clip_level = clip_level;
    stats->clipped_samples = clipped;

    stats->clipped_percent =
        wav->sample_count
            ? ((double)clipped * 100.0) / (double)wav->sample_count
            : 0.0;
}

int detect_initial_signal_state(
    const WavData *wav,
    double threshold
) {
    uint64_t i;

    if (wav->sample_count == 0) {
        return 1;
    }

    for (i = 0; i < wav->sample_count; ++i) {
        double sample = wav->samples[i];

        if (sample >= threshold) {
            return 1;
        }

        if (sample <= -threshold) {
            return 0;
        }
    }

    return wav->samples[0] >= 0.0 ? 1 : 0;
}

void detect_edges_schmitt_range(
    const WavData *wav,
    uint64_t start_sample,
    uint64_t end_sample,
    uint64_t time_unit_ns,
    double threshold,
    uint64_t min_delta_ticks,
    U64List *edges
) {
    int state = 0;
    uint64_t last_edge_tick = 0;
    int have_last_edge = 0;
    uint64_t i;

    if (end_sample > wav->sample_count) {
        end_sample = wav->sample_count;
    }

    for (i = start_sample; i < end_sample; ++i) {
        double sample = wav->samples[i];
        int new_state = state;

        if (state >= 0 && sample < -threshold) {
            new_state = -1;

        } else if (state <= 0 && sample > threshold) {
            new_state = 1;
        }

        if (state == 0) {
            state = new_state;
            continue;
        }

        if (new_state != state) {
            uint64_t tick =
                tsm5_samples_to_ticks(
                    i,
                    wav->sample_rate,
                    time_unit_ns
                );

            if (
                !have_last_edge ||
                tick >= last_edge_tick + min_delta_ticks
            ) {
                u64_list_push(edges, tick);
                last_edge_tick = tick;
                have_last_edge = 1;
                state = new_state;
            }
        }
    }
}

void detect_edges_schmitt(
    const WavData *wav,
    uint64_t time_unit_ns,
    double threshold,
    uint64_t min_delta_ticks,
    U64List *edges
) {
    detect_edges_schmitt_range(
        wav,
        0,
        wav->sample_count,
        time_unit_ns,
        threshold,
        min_delta_ticks,
        edges
    );
}

void build_deltas(
    const U64List *edges,
    U64List *deltas
) {
    uint64_t i;

    if (edges->count < 2) {
        return;
    }

    for (i = 1; i < edges->count; ++i) {
        uint64_t delta = edges->items[i] - edges->items[i - 1];

        if (delta > 0) {
            u64_list_push(deltas, delta);
        }
    }
}

static int compare_delta_rank_desc(
    const void *a,
    const void *b
) {
    const DeltaRank *left = (const DeltaRank *)a;
    const DeltaRank *right = (const DeltaRank *)b;

    if (left->count < right->count) return 1;
    if (left->count > right->count) return -1;
    if (left->delta > right->delta) return 1;
    if (left->delta < right->delta) return -1;

    return 0;
}

DeltaRank *make_delta_ranking(
    const U64List *deltas,
    uint64_t *out_count
) {
    DeltaRank *ranks = NULL;
    uint64_t count = 0;
    uint64_t capacity = 0;
    uint64_t i;

    for (i = 0; i < deltas->count; ++i) {
        uint64_t delta = deltas->items[i];
        uint64_t found = UINT64_MAX;
        uint64_t r;

        for (r = 0; r < count; ++r) {
            if (ranks[r].delta == delta) {
                found = r;
                break;
            }
        }

        if (found != UINT64_MAX) {
            ranks[found].count++;

        } else {
            if (count + 1 > capacity) {
                uint64_t new_capacity = capacity ? capacity * 2 : 256;

                DeltaRank *new_ranks =
                    (DeltaRank *)realloc(
                        ranks,
                        (size_t)new_capacity * sizeof(DeltaRank)
                    );

                if (!new_ranks) {
                    fprintf(stderr, "Out of memory while ranking deltas\n");
                    exit(1);
                }

                ranks = new_ranks;
                capacity = new_capacity;
            }

            ranks[count].delta = delta;
            ranks[count].count = 1;
            count++;
        }
    }

    qsort(ranks, (size_t)count, sizeof(DeltaRank), compare_delta_rank_desc);

    *out_count = count;

    return ranks;
}

static uint64_t abs_diff_u64(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

int nearest_table_index(
    uint64_t delta,
    const uint64_t *table,
    uint32_t table_count,
    uint64_t *out_error
) {
    uint32_t best_index = 0;
    uint64_t best_error = UINT64_MAX;
    uint32_t i;

    for (i = 0; i < table_count; ++i) {
        uint64_t error = abs_diff_u64(delta, table[i]);

        if (error < best_error) {
            best_error = error;
            best_index = i;
        }
    }

    if (out_error) {
        *out_error = best_error;
    }

    return (int)best_index;
}

void build_index_table(
    const DeltaRank *ranks,
    uint64_t rank_count,
    uint64_t *table,
    uint32_t table_count
) {
    uint32_t i;
    uint32_t j;

    for (i = 0; i < table_count; ++i) {
        if ((uint64_t)i < rank_count) {
            table[i] = ranks[i].delta;

        } else {
            table[i] = i > 0 ? table[i - 1] : 1;
        }
    }

    for (i = 0; i < table_count; ++i) {
        for (j = i + 1; j < table_count; ++j) {
            if (table[j] < table[i]) {
                uint64_t temporary = table[i];
                table[i] = table[j];
                table[j] = temporary;
            }
        }
    }
}
