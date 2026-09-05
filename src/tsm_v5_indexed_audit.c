#include "tsm_v5.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    tsm_v5_indexed_audit.c

    Diagnostic tool for TSM v5.1 files.
*/

static void print_u64(uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    printf("%s", text);
}

static uint8_t *read_bytes(FILE *file, uint64_t offset, uint64_t size) {
    uint8_t *data = (uint8_t *)malloc((size_t)size);

    if (!data) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    fseek(file, (long)offset, SEEK_SET);

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "Cannot read payload\n");
        free(data);
        exit(1);
    }

    return data;
}

static uint32_t read_u32_le_from_memory(const uint8_t *data, uint64_t offset) {
    return
        ((uint32_t)data[offset]) |
        ((uint32_t)data[offset + 1] << 8) |
        ((uint32_t)data[offset + 2] << 16) |
        ((uint32_t)data[offset + 3] << 24);
}

static uint32_t *read_delta_table(FILE *file, const TSM5Region *region) {
    uint32_t *table;
    uint32_t i;

    table =
        (uint32_t *)calloc(
            region->table_count,
            sizeof(uint32_t)
        );

    if (!table) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    fseek(file, (long)region->aux_offset, SEEK_SET);

    for (i = 0; i < region->table_count; ++i) {
        uint32_t value = 0;

        if (!tsm5_read_u32(file, &value)) {
            fprintf(stderr, "Cannot read delta table\n");
            free(table);
            exit(1);
        }

        table[i] = value;
    }

    return table;
}

static const char *mode_name(uint32_t mode) {
    if (mode == TSM5_MODE_SILENCE) return "SILENCE/HOLD";
    if (mode == TSM5_MODE_ZERO_SILENCE) return "ZERO_SILENCE";
    if (mode == TSM5_MODE_INDEXED_DELTA) return "INDEXED_DELTA";
    return "UNKNOWN";
}

static const char *encoding_name(uint32_t encoding) {
    if (encoding == TSM5_ENCODING_NONE) return "NONE";
    if (encoding == TSM5_ENCODING_NIBBLE_PACKED_INDEX) return "NIBBLE_PACKED_INDEX";
    if (encoding == TSM5_ENCODING_INDEXED_ESCAPE_BYTE) return "INDEXED_ESCAPE_BYTE";
    if (encoding == TSM5_ENCODING_NIBBLE_ESCAPE_INDEX) return "NIBBLE_ESCAPE_INDEX";
    return "UNKNOWN";
}

static void audit_indexed_region(FILE *file, uint32_t region_index, const TSM5Region *region) {
    uint8_t *payload;
    uint32_t *table;

    uint64_t decoded_delta_count = 0;
    uint64_t indexed_count = 0;
    uint64_t escape_count = 0;
    uint64_t invalid_index_count = 0;
    uint64_t decoded_delta_sum = 0;

    uint32_t i;

    if (region->region_mode != TSM5_MODE_INDEXED_DELTA) {
        return;
    }

    payload = read_bytes(file, region->data_offset, region->data_size);
    table = read_delta_table(file, region);

    if (region->encoding == TSM5_ENCODING_INDEXED_ESCAPE_BYTE) {
        uint64_t offset = 0;

        while (offset < region->data_size) {
            uint8_t code = payload[offset++];
            uint64_t delta = 0;

            if (code == TSM5_ESCAPE_INDEX_BYTE) {
                if (offset + 4 > region->data_size) {
                    printf("region %u malformed escape payload\n", region_index);
                    break;
                }

                delta = read_u32_le_from_memory(payload, offset);
                offset += 4;
                escape_count++;

            } else {
                if (code >= region->table_count) {
                    invalid_index_count++;
                    code = 0;
                }

                delta = table[code];
                indexed_count++;
            }

            decoded_delta_sum += delta;
            decoded_delta_count++;
        }
    }

    printf("  decoded_delta_count: ");
    print_u64(decoded_delta_count);
    printf("\n");

    printf("  indexed_count: ");
    print_u64(indexed_count);
    printf("\n");

    printf("  escape_count: ");
    print_u64(escape_count);
    printf("\n");

    printf("  invalid_index_count: ");
    print_u64(invalid_index_count);
    printf("\n");

    printf("  decoded_delta_sum_ticks: ");
    print_u64(decoded_delta_sum);
    printf("\n");

    printf("  declared_duration_ticks: ");
    print_u64(region->duration_ticks);
    printf("\n");

    if (decoded_delta_sum == region->duration_ticks) {
        printf("  duration_match: YES\n");

    } else if (decoded_delta_sum > region->duration_ticks) {
        printf("  duration_match: NO, decoded sum is LONGER by ");
        print_u64(decoded_delta_sum - region->duration_ticks);
        printf(" ticks\n");

    } else {
        printf("  duration_match: NO, decoded sum is SHORTER by ");
        print_u64(region->duration_ticks - decoded_delta_sum);
        printf(" ticks\n");
    }

    printf("  table:");
    for (i = 0; i < region->table_count; ++i) {
        printf(" ");
        print_u64(table[i]);
    }
    printf("\n");

    free(payload);
    free(table);
}

int main(int argc, char **argv) {
    const char *path;
    FILE *file;

    TSM5Header header;
    TSM5Region *regions;

    uint32_t i;

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s input.tsm\n", argv[0]);
        return 0;
    }

    path = argv[1];

    file = fopen(path, "rb");

    if (!file) {
        fprintf(stderr, "Cannot open TSM: %s\n", path);
        return 1;
    }

    if (!tsm5_read_header(file, &header)) {
        fprintf(stderr, "Invalid TSM v5 file\n");
        fclose(file);
        return 1;
    }

    regions =
        (TSM5Region *)calloc(
            header.region_count,
            sizeof(TSM5Region)
        );

    if (!regions) {
        fprintf(stderr, "Out of memory\n");
        fclose(file);
        return 1;
    }

    for (i = 0; i < header.region_count; ++i) {
        fseek(
            file,
            (long)(header.region_table_offset + (uint64_t)i * header.region_entry_size),
            SEEK_SET
        );

        if (!tsm5_read_region(file, &regions[i])) {
            fprintf(stderr, "Cannot read region %u\n", i);
            fclose(file);
            free(regions);
            return 1;
        }
    }

    printf("TSM v5.1 audit\n");
    printf("==============\n");
    printf("File: %s\n", path);
    printf("region_count: %u\n", header.region_count);
    printf("source_samples: ");
    print_u64(header.reserved1);
    printf("\n");
    printf("source_sample_rate: ");
    print_u64(header.reserved2);
    printf("\n\n");

    for (i = 0; i < header.region_count; ++i) {
        printf("Region %u\n", i);
        printf("--------\n");
        printf("mode: %s (%u)\n", mode_name(regions[i].region_mode), regions[i].region_mode);
        printf("encoding: %s (%u)\n", encoding_name(regions[i].encoding), regions[i].encoding);
        printf("start_ticks: ");
        print_u64(regions[i].start_ticks);
        printf("\n");
        printf("duration_ticks: ");
        print_u64(regions[i].duration_ticks);
        printf("\n");
        printf("data_size: ");
        print_u64(regions[i].data_size);
        printf("\n");

        audit_indexed_region(file, i, &regions[i]);
        printf("\n");
    }

    fclose(file);
    free(regions);

    return 0;
}
