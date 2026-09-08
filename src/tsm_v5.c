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

#include <stdlib.h>
#include <string.h>

static uint32_t tsm5_mem_u32(const uint8_t *data, uint64_t offset) {
    return (uint32_t)data[offset]
         | ((uint32_t)data[offset + 1] << 8)
         | ((uint32_t)data[offset + 2] << 16)
         | ((uint32_t)data[offset + 3] << 24);
}

static uint64_t tsm5_mem_u64(const uint8_t *data, uint64_t offset) {
    return (uint64_t)tsm5_mem_u32(data, offset)
         | ((uint64_t)tsm5_mem_u32(data, offset + 4) << 32);
}

uint32_t tsm5_read_labels(const uint8_t *data, uint64_t size, TSM5Label **out) {
    uint32_t declared;
    uint32_t section_bytes;
    uint64_t start;
    uint64_t end;
    uint64_t at;
    uint32_t found = 0;
    uint32_t pass;
    TSM5Label *labels = NULL;

    if (out) {
        *out = NULL;
    }

    if (!data || size < TSM5_LABEL_FOOTER_SIZE) {
        return 0;
    }

    if (data[size - 4] != 'T' || data[size - 3] != 'S'
     || data[size - 2] != 'M' || data[size - 1] != 'L') {
        return 0;
    }

    declared      = tsm5_mem_u32(data, size - TSM5_LABEL_FOOTER_SIZE);
    section_bytes = tsm5_mem_u32(data, size - TSM5_LABEL_FOOTER_SIZE + 4);

    if ((uint64_t)section_bytes > size - TSM5_LABEL_FOOTER_SIZE) {
        return 0;
    }

    start = size - TSM5_LABEL_FOOTER_SIZE - (uint64_t)section_bytes;
    end   = size - TSM5_LABEL_FOOTER_SIZE;

    /*
        Twice over the same records: once to count what is really there, once
        to build them. What the footer declares is not trusted as a length -
        a truncated file would have it allocating for records it does not
        have.
    */
    for (pass = 0; pass < 2; ++pass) {
        uint32_t n = 0;

        at = start;

        while (at + TSM5_LABEL_HEADER_SIZE <= end && n < declared) {
            uint32_t record_bytes = tsm5_mem_u32(data, at);
            uint32_t text_bytes;

            if (record_bytes < TSM5_LABEL_HEADER_SIZE
             || at + (uint64_t)record_bytes > end) {
                break;
            }

            text_bytes = tsm5_mem_u32(data, at + 28);

            if ((uint64_t)text_bytes > record_bytes - TSM5_LABEL_HEADER_SIZE) {
                text_bytes = record_bytes - TSM5_LABEL_HEADER_SIZE;
            }

            if (pass == 1) {
                char *text = (char *)malloc((size_t)text_bytes + 1);

                if (!text) {
                    tsm5_free_labels(labels, n);
                    return 0;
                }

                if (text_bytes) {
                    memcpy(text, data + at + TSM5_LABEL_HEADER_SIZE, text_bytes);
                }

                text[text_bytes] = 0;

                labels[n].kind       = tsm5_mem_u32(data, at + 4);
                labels[n].region     = tsm5_mem_u32(data, at + 8);
                labels[n].from_ticks = tsm5_mem_u64(data, at + 12);
                labels[n].to_ticks   = tsm5_mem_u64(data, at + 20);
                labels[n].text       = text;
            }

            at += record_bytes;
            n += 1;
        }

        if (pass == 0) {
            found = n;

            if (!found) {
                return 0;
            }

            labels = (TSM5Label *)calloc(found, sizeof(TSM5Label));

            if (!labels) {
                return 0;
            }
        }
    }

    if (out) {
        *out = labels;
    } else {
        tsm5_free_labels(labels, found);
    }

    return found;
}

void tsm5_free_labels(TSM5Label *labels, uint32_t count) {
    uint32_t i;

    if (!labels) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(labels[i].text);
    }

    free(labels);
}

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
