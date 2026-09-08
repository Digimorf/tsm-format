/*
 * TSM - Temporal Signal Medium
 * Reading and writing the TSM v5.2 container.
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

#include <string.h>

void tsm5_write_u16(FILE *file, uint16_t value) {
    fputc((int)(value & 0xff), file);
    fputc((int)((value >> 8) & 0xff), file);
}

void tsm5_write_u32(FILE *file, uint32_t value) {
    int i;

    for (i = 0; i < 4; ++i) {
        fputc((int)((value >> (i * 8)) & 0xff), file);
    }
}

void tsm5_write_u64(FILE *file, uint64_t value) {
    int i;

    for (i = 0; i < 8; ++i) {
        fputc((int)((value >> (i * 8)) & 0xff), file);
    }
}

int tsm5_read_u16(FILE *file, uint16_t *value) {
    int b0 = fgetc(file);
    int b1 = fgetc(file);

    if (b0 < 0 || b1 < 0) {
        return 0;
    }

    *value = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << 8));

    return 1;
}

int tsm5_read_u32(FILE *file, uint32_t *value) {
    uint32_t out = 0;
    int i;

    for (i = 0; i < 4; ++i) {
        int b = fgetc(file);

        if (b < 0) {
            return 0;
        }

        out |= ((uint32_t)b) << (i * 8);
    }

    *value = out;

    return 1;
}

int tsm5_read_u64(FILE *file, uint64_t *value) {
    uint64_t out = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        int b = fgetc(file);

        if (b < 0) {
            return 0;
        }

        out |= ((uint64_t)b) << (i * 8);
    }

    *value = out;

    return 1;
}

int tsm5_write_header(FILE *file, const TSM5Header *header) {
    fwrite(header->magic, 1, 4, file);

    tsm5_write_u16(file, header->version);
    tsm5_write_u16(file, header->header_size);

    tsm5_write_u64(file, header->total_duration_ticks);
    tsm5_write_u64(file, header->base_time_unit_ns);

    tsm5_write_u32(file, header->region_count);
    tsm5_write_u32(file, header->region_entry_size);

    tsm5_write_u64(file, header->region_table_offset);
    tsm5_write_u64(file, header->data_offset);

    tsm5_write_u32(file, header->flags);
    tsm5_write_u32(file, header->reserved32);

    tsm5_write_u64(file, header->reserved0);
    tsm5_write_u64(file, header->reserved1);
    tsm5_write_u64(file, header->reserved2);

    return !ferror(file);
}

int tsm5_read_header(FILE *file, TSM5Header *header) {
    memset(header, 0, sizeof(*header));

    if (fread(header->magic, 1, 4, file) != 4) {
        return 0;
    }

    if (memcmp(header->magic, TSM5_MAGIC, 4) != 0) {
        return 0;
    }

    if (!tsm5_read_u16(file, &header->version)) return 0;
    if (!tsm5_read_u16(file, &header->header_size)) return 0;
    if (!tsm5_read_u64(file, &header->total_duration_ticks)) return 0;
    if (!tsm5_read_u64(file, &header->base_time_unit_ns)) return 0;
    if (!tsm5_read_u32(file, &header->region_count)) return 0;
    if (!tsm5_read_u32(file, &header->region_entry_size)) return 0;
    if (!tsm5_read_u64(file, &header->region_table_offset)) return 0;
    if (!tsm5_read_u64(file, &header->data_offset)) return 0;
    if (!tsm5_read_u32(file, &header->flags)) return 0;
    if (!tsm5_read_u32(file, &header->reserved32)) return 0;
    if (!tsm5_read_u64(file, &header->reserved0)) return 0;
    if (!tsm5_read_u64(file, &header->reserved1)) return 0;
    if (!tsm5_read_u64(file, &header->reserved2)) return 0;

    return header->version == TSM5_VERSION;
}

int tsm5_write_region(FILE *file, const TSM5Region *region) {
    tsm5_write_u64(file, region->start_ticks);
    tsm5_write_u64(file, region->duration_ticks);

    tsm5_write_u32(file, region->region_mode);
    tsm5_write_u32(file, region->encoding);

    tsm5_write_u64(file, region->time_unit_ns);

    tsm5_write_u32(file, region->implicit_event);
    tsm5_write_u32(file, region->index_bits);

    tsm5_write_u32(file, region->table_count);
    tsm5_write_u32(file, region->table_entry_size);

    tsm5_write_u64(file, region->data_offset);
    tsm5_write_u64(file, region->data_size);

    tsm5_write_u64(file, region->aux_offset);
    tsm5_write_u64(file, region->aux_count);

    tsm5_write_u32(file, region->aux_entry_size);
    tsm5_write_u32(file, region->flags);

    tsm5_write_u64(file, region->reserved0);
    tsm5_write_u64(file, region->reserved1);

    return !ferror(file);
}

int tsm5_read_region(FILE *file, TSM5Region *region) {
    memset(region, 0, sizeof(*region));

    if (!tsm5_read_u64(file, &region->start_ticks)) return 0;
    if (!tsm5_read_u64(file, &region->duration_ticks)) return 0;

    if (!tsm5_read_u32(file, &region->region_mode)) return 0;
    if (!tsm5_read_u32(file, &region->encoding)) return 0;

    if (!tsm5_read_u64(file, &region->time_unit_ns)) return 0;

    if (!tsm5_read_u32(file, &region->implicit_event)) return 0;
    if (!tsm5_read_u32(file, &region->index_bits)) return 0;

    if (!tsm5_read_u32(file, &region->table_count)) return 0;
    if (!tsm5_read_u32(file, &region->table_entry_size)) return 0;

    if (!tsm5_read_u64(file, &region->data_offset)) return 0;
    if (!tsm5_read_u64(file, &region->data_size)) return 0;

    if (!tsm5_read_u64(file, &region->aux_offset)) return 0;
    if (!tsm5_read_u64(file, &region->aux_count)) return 0;

    if (!tsm5_read_u32(file, &region->aux_entry_size)) return 0;
    if (!tsm5_read_u32(file, &region->flags)) return 0;

    if (!tsm5_read_u64(file, &region->reserved0)) return 0;
    if (!tsm5_read_u64(file, &region->reserved1)) return 0;

    return 1;
}

uint64_t tsm5_samples_to_ticks(
    uint64_t samples,
    uint32_t sample_rate,
    uint64_t time_unit_ns
) {
    long double ns;
    long double ticks;

    if (sample_rate == 0 || time_unit_ns == 0) {
        return 0;
    }

    ns =
        ((long double)samples * 1000000000.0L) /
        (long double)sample_rate;

    ticks = ns / (long double)time_unit_ns;

    return (uint64_t)(ticks + 0.5L);
}

uint64_t tsm5_ticks_to_samples(
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
) {
    long double ns;
    long double samples;

    if (sample_rate == 0) {
        return 0;
    }

    ns = (long double)ticks * (long double)time_unit_ns;
    samples = ns * (long double)sample_rate / 1000000000.0L;

    return (uint64_t)(samples + 0.5L);
}

void tsm5_u64_to_dec(
    uint64_t value,
    char *buffer,
    unsigned int buffer_size
) {
    char temp[32];
    unsigned int pos = 0;
    unsigned int out_pos = 0;

    if (buffer_size == 0) {
        return;
    }

    if (value == 0) {
        if (buffer_size >= 2) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }

        return;
    }

    while (value != 0 && pos < sizeof(temp)) {
        temp[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0 && out_pos + 1 < buffer_size) {
        buffer[out_pos++] = temp[--pos];
    }

    buffer[out_pos] = '\0';
}
