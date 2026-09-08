/*
 * TSM - Temporal Signal Medium
 * WAV to TSM v5.2: the reference encoder.
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
    wav2tsm_indexed_v5.c

    TSM v5.2 compact converter.

    This version keeps the v5.1 zero-silence segmentation, but changes the
    indexed-delta payload to a compact nibble+escape stream.

    Payload format
    --------------

        nibble 0..14 = delta-table index
        nibble 15    = escape code

    Escape payload:

        escape nibble 15
        raw delta, little-endian, fixed width

    raw_delta_bytes is stored in:

        region.flags

    Default:

        raw_delta_bytes = 4

    Why
    ---

    v5.1 byte-indexed escape was accurate but often about twice the size of
    the original source.  v5.2 makes common deltas compact again:

        common delta = 4 bits
        rare delta   = 4-bit escape + raw numeric delta

    This keeps the timing accuracy while recovering much of the compactness.
*/

#define TSM5_MAX_TABLE_WITH_NIBBLE_ESCAPE 15u
#define DEFAULT_RAW_DELTA_BYTES 4u

typedef struct {
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;

    /*
        When writing nibbles:
            high_half = 0 means next nibble goes to low half
            high_half = 1 means next nibble goes to high half of last byte
    */
    int high_half;
} ByteBuffer;

typedef struct {
    uint64_t start_sample;
    uint64_t end_sample;
} Range;

typedef struct {
    Range *items;
    uint64_t count;
    uint64_t capacity;
} RangeList;

typedef struct {
    TSM5Region region;
    uint8_t *payload;
    uint64_t payload_size;
    uint32_t *table;
    uint32_t table_count;
} OutputRegion;

typedef struct {
    OutputRegion *items;
    uint32_t count;
    uint32_t capacity;
} OutputRegionList;

static void print_u64(uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    printf("%s", text);
}

static void byte_buffer_init(ByteBuffer *buffer) {
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
    buffer->high_half = 0;
}

/*
 * There is no byte_buffer_free(). A payload buffer does not own its memory for
 * long: encode_nibble_escape_payload() hands buffer->data to the region it was
 * built for and the region list owns it from there. A free() beside the init()
 * would look symmetric and be wrong.
 */

static void byte_buffer_reserve(ByteBuffer *buffer, uint64_t required) {
    uint64_t new_capacity;

    if (required <= buffer->capacity) {
        return;
    }

    new_capacity = buffer->capacity ? buffer->capacity * 2 : 65536;

    while (new_capacity < required) {
        new_capacity *= 2;
    }

    buffer->data = (uint8_t *)realloc(buffer->data, (size_t)new_capacity);

    if (!buffer->data) {
        fprintf(stderr, "Out of memory while growing payload\n");
        exit(1);
    }

    buffer->capacity = new_capacity;
}

static void byte_buffer_write_u8(ByteBuffer *buffer, uint8_t value) {
    byte_buffer_reserve(buffer, buffer->size + 1);
    buffer->data[buffer->size++] = value;
}

static void byte_buffer_write_nibble(ByteBuffer *buffer, uint8_t value) {
    value &= 0x0f;

    if (!buffer->high_half) {
        byte_buffer_reserve(buffer, buffer->size + 1);
        buffer->data[buffer->size] = value;
        buffer->size++;
        buffer->high_half = 1;

    } else {
        buffer->data[buffer->size - 1] |= (uint8_t)(value << 4);
        buffer->high_half = 0;
    }
}

static void byte_buffer_align_byte(ByteBuffer *buffer) {
    /*
        If high_half is 1, the low nibble of the last byte has been written and
        the high nibble is still unused.  It remains zero as padding.
    */
    buffer->high_half = 0;
}

static void byte_buffer_write_raw_delta(ByteBuffer *buffer, uint64_t value, uint32_t bytes) {
    uint32_t i;

    byte_buffer_align_byte(buffer);

    for (i = 0; i < bytes; ++i) {
        byte_buffer_write_u8(
            buffer,
            (uint8_t)((value >> (i * 8)) & 0xff)
        );
    }
}

static void range_list_init(RangeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void range_list_free(RangeList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void range_list_push(RangeList *list, uint64_t start, uint64_t end) {
    Range *new_items;

    if (end <= start) {
        return;
    }

    if (list->count + 1 > list->capacity) {
        uint64_t new_capacity = list->capacity ? list->capacity * 2 : 32;

        new_items =
            (Range *)realloc(
                list->items,
                (size_t)new_capacity * sizeof(Range)
            );

        if (!new_items) {
            fprintf(stderr, "Out of memory while growing range list\n");
            exit(1);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count].start_sample = start;
    list->items[list->count].end_sample = end;
    list->count++;
}

static void output_region_list_init(OutputRegionList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void output_region_list_free(OutputRegionList *list) {
    uint32_t i;

    for (i = 0; i < list->count; ++i) {
        free(list->items[i].payload);
        free(list->items[i].table);
    }

    free(list->items);
    memset(list, 0, sizeof(*list));
}

static OutputRegion *output_region_list_push(OutputRegionList *list) {
    OutputRegion *new_items;
    OutputRegion *item;

    if (list->count + 1 > list->capacity) {
        uint32_t new_capacity = list->capacity ? list->capacity * 2 : 32;

        new_items =
            (OutputRegion *)realloc(
                list->items,
                (size_t)new_capacity * sizeof(OutputRegion)
            );

        if (!new_items) {
            fprintf(stderr, "Out of memory while growing output regions\n");
            exit(1);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    item = &list->items[list->count++];
    memset(item, 0, sizeof(*item));

    return item;
}

static void detect_zero_ranges(
    const WavData *wav,
    double zero_threshold,
    double min_zero_ms,
    RangeList *zero_ranges
) {
    uint64_t minimum_samples;
    uint64_t i;
    int in_gap = 0;
    uint64_t gap_start = 0;

    minimum_samples =
        (uint64_t)(
            ((double)wav->sample_rate * min_zero_ms / 1000.0) + 0.5
        );

    if (minimum_samples < 1) {
        minimum_samples = 1;
    }

    for (i = 0; i < wav->sample_count; ++i) {
        int zero = fabs(wav->samples[i]) <= zero_threshold;

        if (zero && !in_gap) {
            in_gap = 1;
            gap_start = i;

        } else if (!zero && in_gap) {
            uint64_t gap_end = i;
            uint64_t duration = gap_end - gap_start;

            if (duration >= minimum_samples) {
                range_list_push(zero_ranges, gap_start, gap_end);
            }

            in_gap = 0;
        }
    }

    if (in_gap) {
        uint64_t gap_end = wav->sample_count;
        uint64_t duration = gap_end - gap_start;

        if (duration >= minimum_samples) {
            range_list_push(zero_ranges, gap_start, gap_end);
        }
    }
}

static int state_at_sample(
    const WavData *wav,
    uint64_t sample_index,
    double edge_threshold
) {
    uint64_t i;

    if (wav->sample_count == 0) {
        return 1;
    }

    if (sample_index >= wav->sample_count) {
        sample_index = wav->sample_count - 1;
    }

    for (i = sample_index; i < wav->sample_count; ++i) {
        if (wav->samples[i] >= edge_threshold) {
            return 1;
        }

        if (wav->samples[i] <= -edge_threshold) {
            return 0;
        }
    }

    return wav->samples[sample_index] >= 0.0 ? 1 : 0;
}

static void add_hold_region(
    OutputRegionList *regions,
    uint64_t start_ticks,
    uint64_t end_ticks,
    uint64_t time_unit_ns,
    int state_high
) {
    OutputRegion *out;

    if (end_ticks <= start_ticks) {
        return;
    }

    out = output_region_list_push(regions);

    out->region.start_ticks = start_ticks;
    out->region.duration_ticks = end_ticks - start_ticks;
    out->region.region_mode = TSM5_MODE_SILENCE;
    out->region.encoding = TSM5_ENCODING_NONE;
    out->region.time_unit_ns = time_unit_ns;
    out->region.implicit_event = TSM5_EVENT_TOGGLE;
    out->region.reserved0 = state_high ? 1 : 0;
}

static void add_zero_region(
    OutputRegionList *regions,
    uint64_t start_ticks,
    uint64_t end_ticks,
    uint64_t time_unit_ns
) {
    OutputRegion *out;

    if (end_ticks <= start_ticks) {
        return;
    }

    out = output_region_list_push(regions);

    out->region.start_ticks = start_ticks;
    out->region.duration_ticks = end_ticks - start_ticks;
    out->region.region_mode = TSM5_MODE_ZERO_SILENCE;
    out->region.encoding = TSM5_ENCODING_NONE;
    out->region.time_unit_ns = time_unit_ns;
    out->region.implicit_event = TSM5_EVENT_TOGGLE;
}

static int exact_table_index(
    uint64_t delta,
    const uint64_t *table,
    uint32_t table_count
) {
    uint32_t i;

    for (i = 0; i < table_count; ++i) {
        if (table[i] == delta) {
            return (int)i;
        }
    }

    return -1;
}

static uint32_t choose_raw_delta_bytes(uint64_t max_delta) {
    if (max_delta <= 0xffULL) {
        return 1;
    }

    if (max_delta <= 0xffffULL) {
        return 2;
    }

    if (max_delta <= 0xffffffULL) {
        return 3;
    }

    return 4;
}

static uint64_t max_unlisted_delta(
    const U64List *deltas,
    const uint64_t *table,
    uint32_t table_count
) {
    uint64_t max_delta = 0;
    uint64_t i;

    for (i = 0; i < deltas->count; ++i) {
        uint64_t delta = deltas->items[i];

        if (exact_table_index(delta, table, table_count) < 0) {
            if (delta > max_delta) {
                max_delta = delta;
            }
        }
    }

    return max_delta;
}

static void encode_nibble_escape_payload(
    const U64List *deltas,
    const uint64_t *table,
    uint32_t table_count,
    uint32_t raw_delta_bytes,
    ByteBuffer *payload,
    uint64_t *indexed_count,
    uint64_t *escape_count
) {
    uint64_t i;
    uint64_t max_raw = 0;

    *indexed_count = 0;
    *escape_count = 0;

    byte_buffer_init(payload);

    if (raw_delta_bytes < 1 || raw_delta_bytes > 4) {
        fprintf(stderr, "ERROR: raw_delta_bytes must be 1..4\n");
        exit(1);
    }

    if (raw_delta_bytes == 1) max_raw = 0xffULL;
    if (raw_delta_bytes == 2) max_raw = 0xffffULL;
    if (raw_delta_bytes == 3) max_raw = 0xffffffULL;
    if (raw_delta_bytes == 4) max_raw = 0xffffffffULL;

    for (i = 0; i < deltas->count; ++i) {
        uint64_t delta = deltas->items[i];
        int index = exact_table_index(delta, table, table_count);

        if (index >= 0) {
            byte_buffer_write_nibble(payload, (uint8_t)index);
            (*indexed_count)++;

        } else {
            if (delta > max_raw) {
                fprintf(stderr, "ERROR: raw delta does not fit selected raw_delta_bytes\n");
                exit(1);
            }

            byte_buffer_write_nibble(payload, TSM5_NIBBLE_ESCAPE_CODE);
            byte_buffer_write_raw_delta(payload, delta, raw_delta_bytes);
            (*escape_count)++;
        }
    }

    byte_buffer_align_byte(payload);
}

static void add_indexed_region_for_active_range(
    OutputRegionList *regions,
    const WavData *wav,
    uint64_t active_start_sample,
    uint64_t active_end_sample,
    uint64_t time_unit_ns,
    double edge_threshold,
    uint64_t min_delta_ticks,
    uint32_t table_count
) {
    U64List edges;
    U64List deltas;
    DeltaRank *ranks;
    uint64_t rank_count = 0;
    uint64_t table64[TSM5_MAX_TABLE_WITH_NIBBLE_ESCAPE];

    ByteBuffer payload;
    uint64_t indexed_count = 0;
    uint64_t escape_count = 0;
    uint64_t max_raw_delta;
    uint32_t raw_delta_bytes;

    uint64_t active_start_ticks;
    uint64_t active_end_ticks;
    uint64_t first_edge;
    uint64_t last_edge;
    int pre_edge_state_high;

    OutputRegion *out;
    uint32_t i;

    if (active_end_sample <= active_start_sample) {
        return;
    }

    active_start_ticks =
        tsm5_samples_to_ticks(
            active_start_sample,
            wav->sample_rate,
            time_unit_ns
        );

    active_end_ticks =
        tsm5_samples_to_ticks(
            active_end_sample,
            wav->sample_rate,
            time_unit_ns
        );

    u64_list_init(&edges);
    u64_list_init(&deltas);

    detect_edges_schmitt_range(
        wav,
        active_start_sample,
        active_end_sample,
        time_unit_ns,
        edge_threshold,
        min_delta_ticks,
        &edges
    );

    if (edges.count < 2) {
        int state_high =
            state_at_sample(
                wav,
                active_start_sample,
                edge_threshold
            );

        add_hold_region(
            regions,
            active_start_ticks,
            active_end_ticks,
            time_unit_ns,
            state_high
        );

        u64_list_free(&edges);
        u64_list_free(&deltas);
        return;
    }

    build_deltas(&edges, &deltas);

    ranks = make_delta_ranking(&deltas, &rank_count);

    memset(table64, 0, sizeof(table64));

    build_index_table(
        ranks,
        rank_count,
        table64,
        table_count
    );

    max_raw_delta =
        max_unlisted_delta(
            &deltas,
            table64,
            table_count
        );

    raw_delta_bytes = choose_raw_delta_bytes(max_raw_delta);

    /*
        For realtime simplicity, allow 4 bytes even when smaller would work.
        For compactness, this converter chooses the smallest safe size per
        region and stores it in region.flags.
    */
    encode_nibble_escape_payload(
        &deltas,
        table64,
        table_count,
        raw_delta_bytes,
        &payload,
        &indexed_count,
        &escape_count
    );

    first_edge = edges.items[0];
    last_edge = edges.items[edges.count - 1];

    pre_edge_state_high =
        state_at_sample(
            wav,
            active_start_sample,
            edge_threshold
        );

    add_hold_region(
        regions,
        active_start_ticks,
        first_edge,
        time_unit_ns,
        pre_edge_state_high
    );

    out = output_region_list_push(regions);

    out->region.start_ticks = first_edge;
    out->region.duration_ticks = last_edge - first_edge;
    out->region.region_mode = TSM5_MODE_INDEXED_DELTA;
    out->region.encoding = TSM5_ENCODING_NIBBLE_ESCAPE_INDEX;
    out->region.time_unit_ns = time_unit_ns;
    out->region.implicit_event = TSM5_EVENT_TOGGLE;
    out->region.index_bits = 4;
    out->region.table_count = table_count;
    out->region.table_entry_size = 4;
    out->region.flags = raw_delta_bytes;
    out->region.reserved0 = pre_edge_state_high ? 1 : 0;

    out->payload = payload.data;
    out->payload_size = payload.size;

    out->table = (uint32_t *)calloc(table_count, sizeof(uint32_t));

    if (!out->table) {
        fprintf(stderr, "Out of memory while storing delta table\n");
        exit(1);
    }

    out->table_count = table_count;

    for (i = 0; i < table_count; ++i) {
        out->table[i] = (uint32_t)table64[i];
    }

    {
        int final_state_high = pre_edge_state_high;

        if (edges.count & 1u) {
            final_state_high = !final_state_high;
        }

        add_hold_region(
            regions,
            last_edge,
            active_end_ticks,
            time_unit_ns,
            final_state_high
        );
    }

    printf("Active region: samples ");
    print_u64(active_start_sample);
    printf("-");
    print_u64(active_end_sample);
    printf(" edges=");
    print_u64(edges.count);
    printf(" deltas=");
    print_u64(deltas.count);
    printf(" indexed=");
    print_u64(indexed_count);
    printf(" escaped=");
    print_u64(escape_count);
    printf(" raw_delta_bytes=%u", raw_delta_bytes);
    printf(" payload_bytes=");
    print_u64(payload.size);
    printf("\n");

    free(ranks);
    u64_list_free(&edges);
    u64_list_free(&deltas);
}

static void build_segmented_regions(
    OutputRegionList *regions,
    const WavData *wav,
    const RangeList *zero_ranges,
    uint64_t time_unit_ns,
    double edge_threshold,
    uint64_t min_delta_ticks,
    uint32_t table_count
) {
    uint64_t cursor_sample = 0;
    uint64_t i;

    for (i = 0; i < zero_ranges->count; ++i) {
        uint64_t zero_start = zero_ranges->items[i].start_sample;
        uint64_t zero_end = zero_ranges->items[i].end_sample;

        if (zero_start > cursor_sample) {
            add_indexed_region_for_active_range(
                regions,
                wav,
                cursor_sample,
                zero_start,
                time_unit_ns,
                edge_threshold,
                min_delta_ticks,
                table_count
            );
        }

        add_zero_region(
            regions,
            tsm5_samples_to_ticks(zero_start, wav->sample_rate, time_unit_ns),
            tsm5_samples_to_ticks(zero_end, wav->sample_rate, time_unit_ns),
            time_unit_ns
        );

        cursor_sample = zero_end;
    }

    if (cursor_sample < wav->sample_count) {
        add_indexed_region_for_active_range(
            regions,
            wav,
            cursor_sample,
            wav->sample_count,
            time_unit_ns,
            edge_threshold,
            min_delta_ticks,
            table_count
        );
    }
}

static void write_tsm_file(
    const char *output_path,
    const WavData *wav,
    const OutputRegionList *regions,
    uint64_t time_unit_ns,
    int initial_state_high
) {
    FILE *file;
    TSM5Header header;
    TSM5Region *table;
    uint64_t current_offset;
    uint32_t i;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, TSM5_MAGIC, 4);

    header.version = TSM5_VERSION;
    header.header_size = TSM5_HEADER_SIZE;
    header.total_duration_ticks =
        tsm5_samples_to_ticks(
            wav->sample_count,
            wav->sample_rate,
            time_unit_ns
        );
    header.base_time_unit_ns = time_unit_ns;
    header.region_count = regions->count;
    header.region_entry_size = TSM5_REGION_SIZE;
    header.region_table_offset = TSM5_HEADER_SIZE;
    header.data_offset =
        TSM5_HEADER_SIZE +
        (uint64_t)regions->count * TSM5_REGION_SIZE;

    header.reserved0 = initial_state_high ? 1 : 0;
    header.reserved1 = wav->sample_count;
    header.reserved2 = wav->sample_rate;

    table =
        (TSM5Region *)calloc(
            regions->count,
            sizeof(TSM5Region)
        );

    if (!table) {
        fprintf(stderr, "Out of memory while preparing region table\n");
        exit(1);
    }

    current_offset = header.data_offset;

    for (i = 0; i < regions->count; ++i) {
        table[i] = regions->items[i].region;

        if (regions->items[i].payload_size > 0) {
            table[i].data_offset = current_offset;
            table[i].data_size = regions->items[i].payload_size;
            current_offset += regions->items[i].payload_size;
        }

        if (regions->items[i].table_count > 0) {
            table[i].aux_offset = current_offset;
            table[i].aux_count = regions->items[i].table_count;
            table[i].aux_entry_size = 4;
            current_offset += (uint64_t)regions->items[i].table_count * 4u;
        }
    }

    file = fopen(output_path, "wb");

    if (!file) {
        fprintf(stderr, "Cannot write TSM: %s\n", output_path);
        exit(1);
    }

    tsm5_write_header(file, &header);

    for (i = 0; i < regions->count; ++i) {
        tsm5_write_region(file, &table[i]);
    }

    for (i = 0; i < regions->count; ++i) {
        if (regions->items[i].payload_size > 0) {
            fwrite(
                regions->items[i].payload,
                1,
                (size_t)regions->items[i].payload_size,
                file
            );
        }

        if (regions->items[i].table_count > 0) {
            uint32_t t;

            for (t = 0; t < regions->items[i].table_count; ++t) {
                tsm5_write_u32(file, regions->items[i].table[t]);
            }
        }
    }

    fclose(file);

    printf("WAV to TSM v5.2 completed\n");
    printf("Encoding: NIBBLE_ESCAPE_INDEX\n");
    printf("Output: %s\n", output_path);
    printf("Regions: %u\n", regions->count);
    printf("Original source samples: ");
    print_u64(wav->sample_count);
    printf("\n");
    printf("Original source sample rate: %u Hz\n", wav->sample_rate);

    free(table);
}

int main(int argc, char **argv) {
    const char *input_wav;
    const char *output_tsm;

    uint64_t time_unit_ns;
    uint32_t requested_table_count;
    uint32_t table_count;
    double edge_threshold;
    uint64_t min_delta_ticks;
    double gain;
    double clip_level;
    double zero_threshold;
    double min_zero_ms;

    WavData wav;
    ConditioningStats stats;
    int initial_state_high;

    RangeList zero_ranges;
    OutputRegionList regions;

    if (argc < 8) {
        printf("Usage:\n");
        printf("  %s input.wav output.tsm time_unit_ns table_count edge_threshold min_delta_ticks gain [clip_level] [zero_threshold] [min_zero_ms]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s input.wav output.tsm 4000 16 0.35 1 8.0 1.0 0.03 20\n", argv[0]);
        return 0;
    }

    input_wav = argv[1];
    output_tsm = argv[2];

    time_unit_ns = (uint64_t)strtoull(argv[3], NULL, 0);
    requested_table_count = (uint32_t)strtoul(argv[4], NULL, 0);
    edge_threshold = atof(argv[5]);
    min_delta_ticks = (uint64_t)strtoull(argv[6], NULL, 0);
    gain = atof(argv[7]);
    clip_level = argc >= 9 ? atof(argv[8]) : 1.0;
    zero_threshold = argc >= 10 ? atof(argv[9]) : 0.03;
    min_zero_ms = argc >= 11 ? atof(argv[10]) : 20.0;

    if (requested_table_count == 4) {
        table_count = 4;
    } else if (requested_table_count == 8) {
        table_count = 8;
    } else if (requested_table_count == 16) {
        table_count = 15;
    } else {
        fprintf(stderr, "ERROR: table_count must be 4, 8, or 16\n");
        return 1;
    }

    if (!wav_load_pcm16_mono_simple(input_wav, &wav)) {
        return 1;
    }

    condition_signal(&wav, gain, clip_level, &stats);

    initial_state_high =
        detect_initial_signal_state(
            &wav,
            edge_threshold
        );

    range_list_init(&zero_ranges);
    output_region_list_init(&regions);

    detect_zero_ranges(
        &wav,
        zero_threshold,
        min_zero_ms,
        &zero_ranges
    );

    printf("Detected zero-silence ranges: ");
    print_u64(zero_ranges.count);
    printf("\n");

    build_segmented_regions(
        &regions,
        &wav,
        &zero_ranges,
        time_unit_ns,
        edge_threshold,
        min_delta_ticks,
        table_count
    );

    write_tsm_file(
        output_tsm,
        &wav,
        &regions,
        time_unit_ns,
        initial_state_high
    );

    output_region_list_free(&regions);
    range_list_free(&zero_ranges);
    wav_free_simple(&wav);

    return 0;
}
