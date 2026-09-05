#include "tsm_v5.h"
#include "wav_io_simple.h"
#include "indexed_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    sc3000_wav_analyzer.c

    Sega SC-3000 cassette WAV analyzer, v2.

    Improvements over v1
    --------------------

    1. Parity is now checked both ways:
        - excluding the key code
        - including the key code

       The SC-3000 chart indicates "asterisked ranges"; from the supplied
       reports, BASIC header parity matches when the key code is excluded.

    2. Header program length is stored and used to evaluate the following
       data block.

    3. Report now shows:
        - expected data bytes from previous header
        - actual decoded data bytes
        - missing/excess byte count
        - first invalid byte-frame position
        - both parity interpretations for diagnostics

    4. The analyzer remains read-only.
*/

#define SC3K_KEY_BASIC_HEADER   0x16
#define SC3K_KEY_BASIC_DATA     0x17
#define SC3K_KEY_MACHINE_HEADER 0x26
#define SC3K_KEY_MACHINE_DATA   0x27

#define SC3K_LEADER_NOMINAL_BITS 3600
#define SC3K_LEADER_MIN_BITS     3000

typedef struct {
    uint8_t value;
    uint64_t start_tick;
    uint64_t end_tick;
    double error;
} DecodedBit;

typedef struct {
    DecodedBit *items;
    uint64_t count;
    uint64_t capacity;
} BitList;

typedef struct {
    uint8_t value;
    uint64_t bit_index;
    uint64_t start_tick;
    uint64_t end_tick;
    int valid_start;
    int valid_stop1;
    int valid_stop2;
    int valid_frame;
} DecodedByte;

typedef struct {
    DecodedByte *items;
    uint64_t count;
    uint64_t capacity;

    int stopped_on_invalid_frame;
    uint64_t invalid_frame_bit_index;
    uint64_t invalid_frame_after_bytes;
    DecodedByte invalid_frame;
} ByteList;

typedef struct {
    uint64_t zero_bits;
    uint64_t one_bits;
    uint64_t unknown_symbols;
} DecodeStats;

typedef struct {
    int have_expected_data_length;
    uint16_t expected_data_length;
    int expected_machine_data;
    char source_label[64];
} AnalyzerState;

static void print_u64_file(FILE *file, uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    fprintf(file, "%s", text);
}

static const char *key_name(uint8_t key) {
    if (key == SC3K_KEY_BASIC_HEADER) return "BASIC TEXT HEADER";
    if (key == SC3K_KEY_BASIC_DATA) return "BASIC TEXT DATA";
    if (key == SC3K_KEY_MACHINE_HEADER) return "MACHINE LANGUAGE HEADER";
    if (key == SC3K_KEY_MACHINE_DATA) return "MACHINE LANGUAGE DATA";
    return "UNKNOWN";
}

static void bit_list_init(BitList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void bit_list_free(BitList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void bit_list_push(
    BitList *list,
    uint8_t value,
    uint64_t start_tick,
    uint64_t end_tick,
    double error
) {
    DecodedBit *new_items;

    if (list->count + 1 > list->capacity) {
        uint64_t new_capacity = list->capacity ? list->capacity * 2 : 8192;

        new_items =
            (DecodedBit *)realloc(
                list->items,
                (size_t)new_capacity * sizeof(DecodedBit)
            );

        if (!new_items) {
            fprintf(stderr, "Out of memory while growing bit list\n");
            exit(1);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count].value = value;
    list->items[list->count].start_tick = start_tick;
    list->items[list->count].end_tick = end_tick;
    list->items[list->count].error = error;
    list->count++;
}

static void byte_list_init(ByteList *list) {
    memset(list, 0, sizeof(*list));
}

static void byte_list_free(ByteList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void byte_list_push(ByteList *list, const DecodedByte *value) {
    DecodedByte *new_items;

    if (list->count + 1 > list->capacity) {
        uint64_t new_capacity = list->capacity ? list->capacity * 2 : 1024;

        new_items =
            (DecodedByte *)realloc(
                list->items,
                (size_t)new_capacity * sizeof(DecodedByte)
            );

        if (!new_items) {
            fprintf(stderr, "Out of memory while growing byte list\n");
            exit(1);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = *value;
}

static uint64_t abs_diff_u64_local(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

static double relative_error(uint64_t measured, uint64_t target) {
    if (target == 0) {
        return 1.0e30;
    }

    return (double)abs_diff_u64_local(measured, target) / (double)target;
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
        error_sum +=
            (long double)relative_error(
                deltas->items[offset + i],
                target
            );
    }

    return (double)(error_sum / (long double)count);
}

static void decode_sc3000_bits(
    const U64List *edges,
    const U64List *deltas,
    BitList *bits,
    uint64_t short_ticks,
    uint64_t long_ticks,
    double max_relative_error,
    DecodeStats *stats
) {
    uint64_t i = 0;

    memset(stats, 0, sizeof(*stats));

    while (i < deltas->count) {
        uint64_t start_tick = edges->items[i];
        double zero_cost = symbol_error(deltas, i, 2, long_ticks);
        double one_cost = symbol_error(deltas, i, 4, short_ticks);

        if (zero_cost <= one_cost && zero_cost <= max_relative_error) {
            uint64_t end_tick =
                edges->items[i] +
                deltas->items[i] +
                deltas->items[i + 1];

            bit_list_push(bits, 0, start_tick, end_tick, zero_cost);
            stats->zero_bits++;
            i += 2;

        } else if (one_cost < zero_cost && one_cost <= max_relative_error) {
            uint64_t end_tick =
                edges->items[i] +
                deltas->items[i] +
                deltas->items[i + 1] +
                deltas->items[i + 2] +
                deltas->items[i + 3];

            bit_list_push(bits, 1, start_tick, end_tick, one_cost);
            stats->one_bits++;
            i += 4;

        } else {
            stats->unknown_symbols++;
            i++;
        }
    }
}

static uint64_t skip_leader_bits(
    const BitList *bits,
    uint64_t start_index,
    uint64_t *out_leader_count
) {
    uint64_t i = start_index;
    uint64_t count = 0;

    while (i < bits->count && bits->items[i].value == 1) {
        count++;
        i++;
    }

    if (out_leader_count) {
        *out_leader_count = count;
    }

    return i;
}

static int decode_one_byte_at(
    const BitList *bits,
    uint64_t bit_index,
    DecodedByte *out
) {
    uint8_t value = 0;
    int b;

    if (bit_index + 11 > bits->count) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    out->bit_index = bit_index;
    out->start_tick = bits->items[bit_index].start_tick;
    out->end_tick = bits->items[bit_index + 10].end_tick;

    out->valid_start = bits->items[bit_index].value == 0;
    out->valid_stop1 = bits->items[bit_index + 9].value == 1;
    out->valid_stop2 = bits->items[bit_index + 10].value == 1;

    for (b = 0; b < 8; ++b) {
        if (bits->items[bit_index + 1 + b].value) {
            value |= (uint8_t)(1u << b);
        }
    }

    out->value = value;
    out->valid_frame =
        out->valid_start &&
        out->valid_stop1 &&
        out->valid_stop2;

    return 1;
}

static uint8_t parity_twos_complement_from_bytes(
    const DecodedByte *bytes,
    uint64_t start,
    uint64_t count
) {
    uint32_t sum = 0;
    uint64_t i;

    for (i = 0; i < count; ++i) {
        sum += bytes[start + i].value;
    }

    return (uint8_t)((0u - sum) & 0xffu);
}

static void print_hex_byte(FILE *file, uint8_t value) {
    static const char hex[] = "0123456789ABCDEF";

    fputc(hex[(value >> 4) & 0x0f], file);
    fputc(hex[value & 0x0f], file);
}

static void print_ascii_name(FILE *file, const DecodedByte *data, uint64_t start, uint64_t count) {
    uint64_t i;

    for (i = 0; i < count; ++i) {
        uint8_t c = data[start + i].value;

        if (c >= 32 && c <= 126) {
            fputc((int)c, file);
        } else {
            fputc('.', file);
        }
    }
}

static uint16_t read_be16_values(uint8_t upper, uint8_t lower) {
    return (uint16_t)(((uint16_t)upper << 8) | lower);
}

static void print_invalid_frame_diagnostic(FILE *report, const ByteList *bytes) {
    if (!bytes->stopped_on_invalid_frame) {
        fprintf(report, "    Framing stop: no invalid frame before parser stop/end.\n");
        return;
    }

    fprintf(report, "    Framing stopped after decoded bytes: ");
    print_u64_file(report, bytes->invalid_frame_after_bytes);
    fprintf(report, "\n");

    fprintf(report, "    Invalid frame bit index: ");
    print_u64_file(report, bytes->invalid_frame_bit_index);
    fprintf(report, "\n");

    fprintf(report, "    Invalid frame details: start=%s stop1=%s stop2=%s value=",
        bytes->invalid_frame.valid_start ? "OK" : "BAD",
        bytes->invalid_frame.valid_stop1 ? "OK" : "BAD",
        bytes->invalid_frame.valid_stop2 ? "OK" : "BAD"
    );
    print_hex_byte(report, bytes->invalid_frame.value);
    fprintf(report, "H\n");
}

static uint64_t decode_byte_stream_after_leader(
    const BitList *bits,
    uint64_t start_bit_index,
    ByteList *bytes
) {
    uint64_t bit_index = start_bit_index;

    while (bit_index + 11 <= bits->count) {
        DecodedByte b;

        if (!decode_one_byte_at(bits, bit_index, &b)) {
            break;
        }

        if (!b.valid_frame) {
            bytes->stopped_on_invalid_frame = 1;
            bytes->invalid_frame_bit_index = bit_index;
            bytes->invalid_frame_after_bytes = bytes->count;
            bytes->invalid_frame = b;
            break;
        }

        byte_list_push(bytes, &b);
        bit_index += 11;

        if (bytes->count > 1024 * 1024) {
            break;
        }
    }

    return bit_index;
}

static void print_parity_diagnostic(
    FILE *report,
    const ByteList *bytes,
    uint64_t parity_index,
    uint64_t exclude_key_start,
    uint64_t exclude_key_count,
    uint64_t include_key_start,
    uint64_t include_key_count
) {
    uint8_t actual;
    uint8_t expected_excluding_key;
    uint8_t expected_including_key;

    if (parity_index >= bytes->count) {
        fprintf(report, "    Parity byte: missing\n");
        return;
    }

    actual = bytes->items[parity_index].value;

    expected_excluding_key =
        parity_twos_complement_from_bytes(
            bytes->items,
            exclude_key_start,
            exclude_key_count
        );

    expected_including_key =
        parity_twos_complement_from_bytes(
            bytes->items,
            include_key_start,
            include_key_count
        );

    fprintf(report, "    Parity byte: ");
    print_hex_byte(report, actual);
    fprintf(report, "H\n");

    fprintf(report, "    Expected parity excluding key code: ");
    print_hex_byte(report, expected_excluding_key);
    fprintf(report, "H -> %s\n", actual == expected_excluding_key ? "OK" : "FAIL");

    fprintf(report, "    Expected parity including key code: ");
    print_hex_byte(report, expected_including_key);
    fprintf(report, "H -> %s\n", actual == expected_including_key ? "OK" : "FAIL");

    if (actual == expected_excluding_key) {
        fprintf(report, "    Preferred parity interpretation: EXCLUDING KEY CODE\n");
    } else if (actual == expected_including_key) {
        fprintf(report, "    Preferred parity interpretation: INCLUDING KEY CODE\n");
    } else {
        fprintf(report, "    Preferred parity interpretation: NONE MATCHED\n");
    }
}

static void analyze_basic_header(FILE *report, const ByteList *bytes, AnalyzerState *state) {
    uint8_t key;
    uint8_t upper;
    uint8_t lower;
    uint16_t length;

    fprintf(report, "    Structure: BASIC TEXT HEADER\n");

    if (bytes->count < 1 + 16 + 2 + 1 + 2) {
        fprintf(report, "    ERROR: too short for BASIC header.\n");
        return;
    }

    key = bytes->items[0].value;
    upper = bytes->items[17].value;
    lower = bytes->items[18].value;

    length = read_be16_values(upper, lower);

    fprintf(report, "    Key code: ");
    print_hex_byte(report, key);
    fprintf(report, "H\n");

    fprintf(report, "    File name: \"");
    print_ascii_name(report, bytes->items, 1, 16);
    fprintf(report, "\"\n");

    fprintf(report, "    Program length: %u bytes\n", (unsigned int)length);

    print_parity_diagnostic(
        report,
        bytes,
        19,
        1,
        18,
        0,
        19
    );

    fprintf(report, "    Dummy data: ");
    if (bytes->count >= 22) {
        print_hex_byte(report, bytes->items[20].value);
        fprintf(report, " ");
        print_hex_byte(report, bytes->items[21].value);
        fprintf(report, "\n");
    } else {
        fprintf(report, "missing\n");
    }

    state->have_expected_data_length = 1;
    state->expected_data_length = length;
    state->expected_machine_data = 0;
    snprintf(state->source_label, sizeof(state->source_label), "BASIC text header");
}

static void analyze_machine_header(FILE *report, const ByteList *bytes, AnalyzerState *state) {
    uint8_t key;
    uint8_t length_upper;
    uint8_t length_lower;
    uint8_t start_upper;
    uint8_t start_lower;
    uint16_t length;
    uint16_t start_address;

    fprintf(report, "    Structure: MACHINE LANGUAGE HEADER\n");

    if (bytes->count < 1 + 16 + 2 + 2 + 1 + 2) {
        fprintf(report, "    ERROR: too short for machine-language header.\n");
        return;
    }

    key = bytes->items[0].value;
    length_upper = bytes->items[17].value;
    length_lower = bytes->items[18].value;
    start_upper = bytes->items[19].value;
    start_lower = bytes->items[20].value;

    length = read_be16_values(length_upper, length_lower);
    start_address = read_be16_values(start_upper, start_lower);

    fprintf(report, "    Key code: ");
    print_hex_byte(report, key);
    fprintf(report, "H\n");

    fprintf(report, "    File name: \"");
    print_ascii_name(report, bytes->items, 1, 16);
    fprintf(report, "\"\n");

    fprintf(report, "    Program length: %u bytes\n", (unsigned int)length);

    fprintf(report, "    Start address: %u decimal / ", (unsigned int)start_address);
    print_hex_byte(report, (uint8_t)(start_address >> 8));
    print_hex_byte(report, (uint8_t)(start_address & 0xff));
    fprintf(report, "H\n");

    print_parity_diagnostic(
        report,
        bytes,
        21,
        1,
        20,
        0,
        21
    );

    fprintf(report, "    Dummy data: ");
    if (bytes->count >= 24) {
        print_hex_byte(report, bytes->items[22].value);
        fprintf(report, " ");
        print_hex_byte(report, bytes->items[23].value);
        fprintf(report, "\n");
    } else {
        fprintf(report, "missing\n");
    }

    state->have_expected_data_length = 1;
    state->expected_data_length = length;
    state->expected_machine_data = 1;
    snprintf(state->source_label, sizeof(state->source_label), "machine-language header");
}

static void analyze_data_block(FILE *report, const ByteList *bytes, AnalyzerState *state, int machine) {
    uint8_t key;
    uint64_t data_count;
    uint64_t expected_total_min;

    fprintf(report, "    Structure: %s DATA\n", machine ? "MACHINE LANGUAGE" : "BASIC TEXT");

    if (bytes->count < 1 + 1 + 2) {
        fprintf(report, "    ERROR: too short for data block.\n");
        return;
    }

    key = bytes->items[0].value;

    data_count = bytes->count >= 4 ? bytes->count - 4 : 0;

    fprintf(report, "    Key code: ");
    print_hex_byte(report, key);
    fprintf(report, "H\n");

    fprintf(report, "    Data bytes seen: ");
    print_u64_file(report, data_count);
    fprintf(report, "\n");

    if (state->have_expected_data_length) {
        uint64_t expected = state->expected_data_length;

        fprintf(report, "    Expected data bytes from previous %s: ", state->source_label);
        print_u64_file(report, expected);
        fprintf(report, "\n");

        if (data_count == expected) {
            fprintf(report, "    Data length check: OK\n");
        } else if (data_count < expected) {
            fprintf(report, "    Data length check: SHORT by ");
            print_u64_file(report, expected - data_count);
            fprintf(report, " bytes\n");
        } else {
            fprintf(report, "    Data length check: LONG by ");
            print_u64_file(report, data_count - expected);
            fprintf(report, " bytes\n");
        }

        expected_total_min = 1 + expected + 1 + 2;
        fprintf(report, "    Expected minimum block bytes including key/parity/dummy: ");
        print_u64_file(report, expected_total_min);
        fprintf(report, "\n");
    } else {
        fprintf(report, "    Expected data bytes: unavailable, no previous header length found.\n");
    }

    /*
        For the bytes we have, assume the last three are parity + dummy + dummy.
    */
    print_parity_diagnostic(
        report,
        bytes,
        1 + data_count,
        1,
        data_count,
        0,
        1 + data_count
    );

    fprintf(report, "    Dummy data: ");
    if (bytes->count >= 3) {
        print_hex_byte(report, bytes->items[bytes->count - 2].value);
        fprintf(report, " ");
        print_hex_byte(report, bytes->items[bytes->count - 1].value);
        fprintf(report, "\n");
    } else {
        fprintf(report, "missing\n");
    }
}

static void print_byte_preview(FILE *report, const ByteList *bytes, uint64_t max_count) {
    uint64_t i;
    uint64_t count = bytes->count < max_count ? bytes->count : max_count;

    fprintf(report, "    Byte preview:");

    for (i = 0; i < count; ++i) {
        fprintf(report, " ");
        print_hex_byte(report, bytes->items[i].value);
    }

    if (bytes->count > count) {
        fprintf(report, " ...");
    }

    fprintf(report, "\n");
}

static void analyze_block(
    FILE *report,
    const BitList *bits,
    uint64_t block_number,
    uint64_t leader_start_bit,
    uint64_t leader_count,
    uint64_t data_start_bit,
    AnalyzerState *state
) {
    ByteList bytes;
    uint8_t key = 0;
    uint64_t block_end_bit;

    byte_list_init(&bytes);

    block_end_bit =
        decode_byte_stream_after_leader(
            bits,
            data_start_bit,
            &bytes
        );

    fprintf(report, "\nBLOCK ");
    print_u64_file(report, block_number);
    fprintf(report, "\n");
    fprintf(report, "------------------------------------------------------------\n");

    fprintf(report, "    Leader start bit index: ");
    print_u64_file(report, leader_start_bit);
    fprintf(report, "\n");

    fprintf(report, "    Leader bit-1 count: ");
    print_u64_file(report, leader_count);
    fprintf(report, "\n");

    fprintf(report, "    Leader nominal: %u\n", SC3K_LEADER_NOMINAL_BITS);
    fprintf(report, "    Leader status: %s\n", leader_count >= SC3K_LEADER_MIN_BITS ? "OK" : "SHORT");

    fprintf(report, "    Data start bit index: ");
    print_u64_file(report, data_start_bit);
    fprintf(report, "\n");

    fprintf(report, "    Data end bit index: ");
    print_u64_file(report, block_end_bit);
    fprintf(report, "\n");

    fprintf(report, "    Decoded bytes: ");
    print_u64_file(report, bytes.count);
    fprintf(report, "\n");

    print_invalid_frame_diagnostic(report, &bytes);

    if (bytes.count == 0) {
        fprintf(report, "    ERROR: no decodable byte frames after leader.\n");
        byte_list_free(&bytes);
        return;
    }

    key = bytes.items[0].value;

    fprintf(report, "    Key code: ");
    print_hex_byte(report, key);
    fprintf(report, "H - %s\n", key_name(key));

    print_byte_preview(report, &bytes, 40);

    if (key == SC3K_KEY_BASIC_HEADER) {
        analyze_basic_header(report, &bytes, state);

    } else if (key == SC3K_KEY_BASIC_DATA) {
        analyze_data_block(report, &bytes, state, 0);

    } else if (key == SC3K_KEY_MACHINE_HEADER) {
        analyze_machine_header(report, &bytes, state);

    } else if (key == SC3K_KEY_MACHINE_DATA) {
        analyze_data_block(report, &bytes, state, 1);

    } else {
        fprintf(report, "    Structure: UNKNOWN\n");
    }

    byte_list_free(&bytes);
}

static void scan_and_analyze_blocks(FILE *report, const BitList *bits) {
    uint64_t i = 0;
    uint64_t block_number = 0;
    AnalyzerState state;

    memset(&state, 0, sizeof(state));

    while (i < bits->count) {
        uint64_t leader_count = 0;
        uint64_t leader_start = i;
        uint64_t after_leader;

        if (bits->items[i].value != 1) {
            i++;
            continue;
        }

        after_leader = skip_leader_bits(bits, i, &leader_count);

        if (leader_count >= SC3K_LEADER_MIN_BITS) {
            block_number++;

            analyze_block(
                report,
                bits,
                block_number,
                leader_start,
                leader_count,
                after_leader,
                &state
            );

            /*
                Move past this leader. We do not know exact block length after
                a framing loss, so continue scanning from after_leader + 11 to
                avoid detecting the same leader again.
            */
            i = after_leader + 11;

        } else {
            i = after_leader;
        }
    }

    fprintf(report, "\nDetected SC-3000-like blocks: ");
    print_u64_file(report, block_number);
    fprintf(report, "\n");
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *report_path;

    uint64_t time_unit_ns;
    double edge_threshold;
    uint64_t min_delta_ticks;
    double gain;
    double clip_level;
    uint64_t short_ticks;
    uint64_t long_ticks;
    double max_relative_error;

    WavData wav;
    ConditioningStats conditioning;
    U64List edges;
    U64List deltas;
    BitList bits;
    DecodeStats stats;

    FILE *report;

    if (argc < 11) {
        printf("Usage:\n");
        printf("  %s input.wav report.txt time_unit_ns edge_threshold min_delta_ticks gain clip_level short_ticks long_ticks max_relative_error\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s input.wav report.txt 4000 0.35 1 8.0 1.0 52 104 0.35\n", argv[0]);
        return 0;
    }

    input_path = argv[1];
    report_path = argv[2];

    time_unit_ns = (uint64_t)strtoull(argv[3], NULL, 0);
    edge_threshold = atof(argv[4]);
    min_delta_ticks = (uint64_t)strtoull(argv[5], NULL, 0);
    gain = atof(argv[6]);
    clip_level = atof(argv[7]);
    short_ticks = (uint64_t)strtoull(argv[8], NULL, 0);
    long_ticks = (uint64_t)strtoull(argv[9], NULL, 0);
    max_relative_error = atof(argv[10]);

    if (!wav_load_pcm16_mono_simple(input_path, &wav)) {
        return 1;
    }

    condition_signal(
        &wav,
        gain,
        clip_level,
        &conditioning
    );

    u64_list_init(&edges);
    u64_list_init(&deltas);
    bit_list_init(&bits);

    detect_edges_schmitt(
        &wav,
        time_unit_ns,
        edge_threshold,
        min_delta_ticks,
        &edges
    );

    build_deltas(&edges, &deltas);

    decode_sc3000_bits(
        &edges,
        &deltas,
        &bits,
        short_ticks,
        long_ticks,
        max_relative_error,
        &stats
    );

    report = fopen(report_path, "wb");

    if (!report) {
        fprintf(stderr, "Cannot write report: %s\n", report_path);
        bit_list_free(&bits);
        u64_list_free(&edges);
        u64_list_free(&deltas);
        wav_free_simple(&wav);
        return 1;
    }

    fprintf(report, "SC-3000 WAV Analyzer Report v2\n");
    fprintf(report, "============================================================\n\n");

    fprintf(report, "Input WAV\n");
    fprintf(report, "---------\n");
    fprintf(report, "File: %s\n", input_path);
    fprintf(report, "Sample rate: %u Hz\n", wav.sample_rate);
    fprintf(report, "Channels: %u\n", wav.channels);
    fprintf(report, "Samples: ");
    print_u64_file(report, wav.sample_count);
    fprintf(report, "\n");
    fprintf(report, "Duration: %.6f seconds\n",
        wav.sample_rate ? (double)wav.sample_count / (double)wav.sample_rate : 0.0
    );

    fprintf(report, "\nSignal conditioning\n");
    fprintf(report, "-------------------\n");
    fprintf(report, "DC offset removed: %.8f\n", conditioning.dc_offset);
    fprintf(report, "Peak before gain: %.8f\n", conditioning.peak_before_gain);
    fprintf(report, "Gain: %.6f\n", gain);
    fprintf(report, "Clip level: %.6f\n", clip_level);

    fprintf(report, "\nTiming parameters\n");
    fprintf(report, "-----------------\n");
    fprintf(report, "time_unit_ns: ");
    print_u64_file(report, time_unit_ns);
    fprintf(report, "\n");
    fprintf(report, "edge_threshold: +/-%.6f\n", edge_threshold);
    fprintf(report, "min_delta_ticks: ");
    print_u64_file(report, min_delta_ticks);
    fprintf(report, "\n");
    fprintf(report, "short_ticks 2400 Hz half-period: ");
    print_u64_file(report, short_ticks);
    fprintf(report, "\n");
    fprintf(report, "long_ticks 1200 Hz half-period: ");
    print_u64_file(report, long_ticks);
    fprintf(report, "\n");
    fprintf(report, "max_relative_error: %.6f\n", max_relative_error);

    fprintf(report, "\nLow-level decode\n");
    fprintf(report, "----------------\n");
    fprintf(report, "Edges: ");
    print_u64_file(report, edges.count);
    fprintf(report, "\n");
    fprintf(report, "Deltas: ");
    print_u64_file(report, deltas.count);
    fprintf(report, "\n");
    fprintf(report, "Decoded bits: ");
    print_u64_file(report, bits.count);
    fprintf(report, "\n");
    fprintf(report, "Bit 0 count: ");
    print_u64_file(report, stats.zero_bits);
    fprintf(report, "\n");
    fprintf(report, "Bit 1 count: ");
    print_u64_file(report, stats.one_bits);
    fprintf(report, "\n");
    fprintf(report, "Unknown timing symbols skipped: ");
    print_u64_file(report, stats.unknown_symbols);
    fprintf(report, "\n");

    fprintf(report, "\nSC-3000 block analysis\n");
    fprintf(report, "============================================================\n");

    scan_and_analyze_blocks(report, &bits);

    fclose(report);

    printf("SC-3000 WAV analysis v2 completed\n");
    printf("Input: %s\n", input_path);
    printf("Report: %s\n", report_path);
    printf("Decoded bits: ");
    {
        char text[32];
        tsm5_u64_to_dec(bits.count, text, sizeof(text));
        printf("%s\n", text);
    }

    bit_list_free(&bits);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
