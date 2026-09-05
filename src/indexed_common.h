#ifndef INDEXED_COMMON_H
#define INDEXED_COMMON_H

#include "wav_io_simple.h"
#include "tsm_v5.h"

#include <stdint.h>

typedef struct {
    uint64_t *items;
    uint64_t count;
    uint64_t capacity;
} U64List;

typedef struct {
    double dc_offset;
    double peak_before_gain;
    double gain;
    double clip_level;
    uint64_t clipped_samples;
    double clipped_percent;
} ConditioningStats;

typedef struct {
    uint64_t delta;
    uint64_t count;
} DeltaRank;

void u64_list_init(U64List *list);
void u64_list_free(U64List *list);
void u64_list_push(U64List *list, uint64_t value);

void condition_signal(
    WavData *wav,
    double gain,
    double clip_level,
    ConditioningStats *stats
);

int detect_initial_signal_state(
    const WavData *wav,
    double threshold
);

void detect_edges_schmitt_range(
    const WavData *wav,
    uint64_t start_sample,
    uint64_t end_sample,
    uint64_t time_unit_ns,
    double threshold,
    uint64_t min_delta_ticks,
    U64List *edges
);

void detect_edges_schmitt(
    const WavData *wav,
    uint64_t time_unit_ns,
    double threshold,
    uint64_t min_delta_ticks,
    U64List *edges
);

void build_deltas(
    const U64List *edges,
    U64List *deltas
);

DeltaRank *make_delta_ranking(
    const U64List *deltas,
    uint64_t *out_count
);

void build_index_table(
    const DeltaRank *ranks,
    uint64_t rank_count,
    uint64_t *table,
    uint32_t table_count
);

int nearest_table_index(
    uint64_t delta,
    const uint64_t *table,
    uint32_t table_count,
    uint64_t *out_error
);

#endif
