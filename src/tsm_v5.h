/*
 * TSM - Temporal Signal Medium
 * The TSM v5.2 container: header, region table, primitives.
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

#ifndef TSM_V5_H
#define TSM_V5_H

/*
    TSM v5.1

    TSM means Temporal Signal Medium.

    This is a generic temporal signal format.  It is not tied to any specific
    machine, protocol, or historical source.

    Main concepts:
        - source signal
        - edge stream
        - indexed delta table
        - raw-delta escape for uncommon deltas
        - explicit zero-silence regions between active blocks
*/

#include <stdint.h>
#include <stdio.h>

#define TSM5_MAGIC "TSM5"
#define TSM5_VERSION 5

#define TSM5_HEADER_SIZE 80
#define TSM5_REGION_SIZE 104

#define TSM5_MODE_SILENCE        0   /* Hold current HIGH/LOW state. */
#define TSM5_MODE_INDEXED_DELTA  3   /* Edge/toggle stream. */
#define TSM5_MODE_ZERO_SILENCE   4   /* True zero-amplitude silence. */

#define TSM5_ENCODING_NONE                   0
#define TSM5_ENCODING_NIBBLE_PACKED_INDEX    5
#define TSM5_ENCODING_INDEXED_ESCAPE_BYTE    6
#define TSM5_ENCODING_NIBBLE_ESCAPE_INDEX   7

#define TSM5_EVENT_TOGGLE 1
#define TSM5_ESCAPE_INDEX_BYTE 255u
#define TSM5_NIBBLE_ESCAPE_CODE 15u

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t header_size;

    uint64_t total_duration_ticks;
    uint64_t base_time_unit_ns;

    uint32_t region_count;
    uint32_t region_entry_size;

    uint64_t region_table_offset;
    uint64_t data_offset;

    uint32_t flags;
    uint32_t reserved32;

    /*
        Experimental v5 metadata:

            reserved0 = initial signal state
                0 = LOW
                1 = HIGH

            reserved1 = original source sample count

            reserved2 = original source sample rate
    */
    uint64_t reserved0;
    uint64_t reserved1;
    uint64_t reserved2;
} TSM5Header;

typedef struct {
    uint64_t start_ticks;
    uint64_t duration_ticks;

    uint32_t region_mode;
    uint32_t encoding;

    uint64_t time_unit_ns;

    uint32_t implicit_event;
    uint32_t index_bits;

    uint32_t table_count;
    uint32_t table_entry_size;

    uint64_t data_offset;
    uint64_t data_size;

    uint64_t aux_offset;
    uint64_t aux_count;

    uint32_t aux_entry_size;
    uint32_t flags;

    /*
        For SILENCE and INDEXED_DELTA regions:

            reserved0 = state before/through region
                0 = LOW
                1 = HIGH
    */
    uint64_t reserved0;
    uint64_t reserved1;
} TSM5Region;

void tsm5_write_u16(FILE *file, uint16_t value);
void tsm5_write_u32(FILE *file, uint32_t value);
void tsm5_write_u64(FILE *file, uint64_t value);

int tsm5_read_u16(FILE *file, uint16_t *value);
int tsm5_read_u32(FILE *file, uint32_t *value);
int tsm5_read_u64(FILE *file, uint64_t *value);

int tsm5_write_header(FILE *file, const TSM5Header *header);
int tsm5_read_header(FILE *file, TSM5Header *header);

int tsm5_write_region(FILE *file, const TSM5Region *region);
int tsm5_read_region(FILE *file, TSM5Region *region);

uint64_t tsm5_samples_to_ticks(
    uint64_t samples,
    uint32_t sample_rate,
    uint64_t time_unit_ns
);

uint64_t tsm5_ticks_to_samples(
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
);

void tsm5_u64_to_dec(
    uint64_t value,
    char *buffer,
    unsigned int buffer_size
);

#endif
