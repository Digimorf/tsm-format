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

/*
    The label section.

    A TSM is a stream of edges and nothing else, which is the point: it does
    not know what the tape carries. That leaves nowhere to put what a person
    worked out about it - what the programs are called, which system wrote
    each region, where the recording came from, what has to be typed to load
    it - and that knowledge was being lost with every file written.

    The section is appended AFTER the last region payload and found from the
    end of the file:

        [ ... region payloads ... ][record][record] ... [u32 count][u32 bytes]["TSML"]

    Nothing that comes before it moves. A reader that knows nothing of this
    never looks at the end of the file and never notices; a reader that wants
    the labels seeks twelve bytes from the end, checks the magic, and walks
    back. There is deliberately no flag in the header saying the section is
    there: that would be a second thing to keep true, and a tool that rewrote
    the payloads and dropped the section would leave it lying.

    One record:

        u32 record_bytes    including this field, padded to 4
        u32 kind            see below; an unknown kind is stepped over
        u32 region          a region index, or TSM5_LABEL_TAPE
        u64 from_ticks      the counter the label runs between, in the file's
        u64 to_ticks        own base time unit
        u32 text_bytes
        u8  text[]          UTF-8, not terminated

    Because a record carries its own length, the list of kinds can grow
    without any existing reader having to change.
*/

#define TSM5_LABEL_HEADER_SIZE 32u
#define TSM5_LABEL_FOOTER_SIZE 12u

#define TSM5_LABEL_NAME    1u   /* what the program in this region is called */
#define TSM5_LABEL_PROFILE 2u   /* which system wrote it, as a short token   */
#define TSM5_LABEL_ORIGIN  3u   /* where the recording came from, and when   */
#define TSM5_LABEL_LOAD    4u   /* what has to be done to load it            */

/* A record about the whole tape rather than about one region. */
#define TSM5_LABEL_TAPE    0xFFFFFFFFu

typedef struct {
    uint32_t kind;
    uint32_t region;
    uint64_t from_ticks;
    uint64_t to_ticks;
    char *text;             /* NUL-terminated; owned by the array */
} TSM5Label;

/*
    Read the label section of a TSM held in memory.

    Memory and not FILE, unlike the rest of this header: the section is found
    from the end of the file, and every caller that wants it - a player, an
    indexer, a browser - already has the bytes.

    data  the whole TSM, exactly as long as it is
    size  its length
    out   receives a malloc'd array of `count` labels, or NULL

    Returns the number of labels. Zero for a file with no section, a truncated
    one, or one whose footer does not add up; saying nothing is the ordinary
    case, not an error.
*/
uint32_t tsm5_read_labels(const uint8_t *data, uint64_t size, TSM5Label **out);

/* Release what tsm5_read_labels() handed back. */
void tsm5_free_labels(TSM5Label *labels, uint32_t count);

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
