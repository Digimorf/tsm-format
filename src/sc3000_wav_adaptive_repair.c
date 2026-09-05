#include "tsm_v5.h"
#include "wav_io_simple.h"
#include "indexed_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    sc3000_wav_adaptive_repair.c

    Sega SC-3000 adaptive WAV reader/repair tool.

    Goal
    ----

    This tool uses the SC-3000 tape structure itself to recover from timing
    drift, drag, and local framing damage.

    Strategy
    --------

    1. Detect edges.
    2. Find leader runs made of 2400 Hz half-periods.
    3. Estimate the local short half-period from the leader.
    4. Decode bytes using the known SC-3000 serial frame:

            start bit = 0
            8 data bits LSB-first
            stop bit = 1
            stop bit = 1

    5. Use expected key/header/data structure:
            16H = BASIC header
            17H = BASIC data
            26H = machine header
            27H = machine data

    6. If a previous header provides program length, read the following
       data block for the expected number of bytes instead of stopping at the
       first imperfect frame.

    7. Render a clean repaired WAV.

    Important
    ---------

    This is a REPAIR tool, not an archival tool.

    It may produce a cleaner file than the source, but it is not meant to
    preserve the original waveform imperfections.
*/

#define SC3K_KEY_BASIC_HEADER   0x16
#define SC3K_KEY_BASIC_DATA     0x17
#define SC3K_KEY_MACHINE_HEADER 0x26
#define SC3K_KEY_MACHINE_DATA   0x27

#define SC3K_MIN_LEADER_BITS    3000
#define SC3K_NOMINAL_LEADER_BITS 3600

typedef struct {
    int16_t *samples;
    uint64_t count;
    uint64_t capacity;
} AudioBuffer;

typedef struct {
    uint8_t *data;
    uint64_t count;
    uint64_t capacity;
} ByteBuffer;

typedef struct {
    uint64_t leader_start_delta;
    uint64_t leader_half_count;
    uint64_t leader_bits;
    uint64_t data_delta_index;
    uint64_t leader_start_tick;
    uint64_t data_start_tick;
    uint64_t data_end_tick;
    double short_ticks;
    double base_ticks;
    uint64_t decoded_bytes;
    uint64_t forced_frames;
    uint64_t weak_frames;
    uint8_t key;
} BlockInfo;

typedef struct {
    int have_expected_length;
    uint16_t expected_length;
    int expected_machine;
} ExpectedState;

static void print_u64(FILE *file, uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    fprintf(file, "%s", text);
}

static void audio_init(AudioBuffer *audio) {
    memset(audio, 0, sizeof(*audio));
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

    audio->samples = (int16_t *)realloc(audio->samples, (size_t)new_capacity * sizeof(int16_t));

    if (!audio->samples) {
        fprintf(stderr, "Out of memory growing audio buffer\n");
        exit(1);
    }

    audio->capacity = new_capacity;
}

static void audio_append(AudioBuffer *audio, int16_t value, uint64_t count) {
    uint64_t i;

    if (count == 0) {
        return;
    }

    audio_reserve(audio, audio->count + count);

    for (i = 0; i < count; ++i) {
        audio->samples[audio->count++] = value;
    }
}

static void byte_buffer_init(ByteBuffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
}

static void byte_buffer_free(ByteBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void byte_buffer_push(ByteBuffer *buffer, uint8_t value) {
    if (buffer->count + 1 > buffer->capacity) {
        uint64_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 1024;
        uint8_t *new_data = (uint8_t *)realloc(buffer->data, (size_t)new_capacity);

        if (!new_data) {
            fprintf(stderr, "Out of memory growing byte buffer\n");
            exit(1);
        }

        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }

    buffer->data[buffer->count++] = value;
}

static double abs_double(double x) {
    return x < 0.0 ? -x : x;
}

static double mean_deltas(
    const U64List *deltas,
    uint64_t start,
    uint64_t count
) {
    long double sum = 0.0L;
    uint64_t i;

    if (count == 0 || start >= deltas->count) {
        return 52.0;
    }

    if (start + count > deltas->count) {
        count = deltas->count - start;
    }

    for (i = 0; i < count; ++i) {
        sum += (long double)deltas->items[start + i];
    }

    return (double)(sum / (long double)count);
}

static double rel_error_double(double measured, double target) {
    if (target <= 0.0) {
        return 999999.0;
    }

    return abs_double(measured - target) / target;
}

static int is_short_delta(
    uint64_t d,
    double short_ticks,
    double tolerance
) {
    return rel_error_double((double)d, short_ticks) <= tolerance;
}

static uint64_t find_next_leader(
    const U64List *edges,
    const U64List *deltas,
    uint64_t start_delta,
    double seed_short,
    double tolerance,
    BlockInfo *out
) {
    uint64_t i = start_delta;

    while (i < deltas->count) {
        uint64_t run_start = i;
        uint64_t run_count = 0;
        double local_short = seed_short;

        while (
            i < deltas->count &&
            is_short_delta(deltas->items[i], local_short, tolerance)
        ) {
            /*
                Slow adaptation while scanning the leader.
            */
            local_short = (local_short * 0.995) + ((double)deltas->items[i] * 0.005);
            run_count++;
            i++;
        }

        if (run_count >= SC3K_MIN_LEADER_BITS * 4ULL) {
            memset(out, 0, sizeof(*out));

            out->leader_start_delta = run_start;
            out->leader_half_count = run_count;
            out->leader_bits = run_count / 4;
            out->data_delta_index = run_start + out->leader_bits * 4;
            out->leader_start_tick = edges->items[run_start];
            out->data_start_tick = edges->items[out->data_delta_index];
            out->short_ticks = mean_deltas(deltas, run_start, out->leader_bits * 4);
            out->base_ticks = out->short_ticks;

            return out->data_delta_index;
        }

        i++;
    }

    return UINT64_MAX;
}

static uint64_t ticks_to_samples_local(uint64_t ticks, uint64_t time_unit_ns, uint32_t sample_rate) {
    return tsm5_ticks_to_samples(ticks, time_unit_ns, sample_rate);
}

static void render_ticks(
    AudioBuffer *audio,
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int level,
    int16_t amplitude
) {
    uint64_t samples = ticks_to_samples_local(ticks, time_unit_ns, sample_rate);

    audio_append(audio, level > 0 ? amplitude : (int16_t)-amplitude, samples);
}

static void render_zero_ticks(
    AudioBuffer *audio,
    uint64_t ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate
) {
    uint64_t samples = ticks_to_samples_local(ticks, time_unit_ns, sample_rate);

    audio_append(audio, 0, samples);
}

static void render_bit(
    AudioBuffer *audio,
    int bit,
    double base_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int16_t amplitude
) {
    uint64_t short_ticks = (uint64_t)(base_ticks + 0.5);
    uint64_t long_ticks = (uint64_t)((base_ticks * 2.0) + 0.5);

    if (bit) {
        render_ticks(audio, short_ticks, time_unit_ns, sample_rate,  1, amplitude);
        render_ticks(audio, short_ticks, time_unit_ns, sample_rate, -1, amplitude);
        render_ticks(audio, short_ticks, time_unit_ns, sample_rate,  1, amplitude);
        render_ticks(audio, short_ticks, time_unit_ns, sample_rate, -1, amplitude);
    } else {
        render_ticks(audio, long_ticks, time_unit_ns, sample_rate,  1, amplitude);
        render_ticks(audio, long_ticks, time_unit_ns, sample_rate, -1, amplitude);
    }
}

static void render_byte(
    AudioBuffer *audio,
    uint8_t value,
    double base_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int16_t amplitude
) {
    int i;

    render_bit(audio, 0, base_ticks, time_unit_ns, sample_rate, amplitude);

    for (i = 0; i < 8; ++i) {
        render_bit(audio, (value >> i) & 1, base_ticks, time_unit_ns, sample_rate, amplitude);
    }

    render_bit(audio, 1, base_ticks, time_unit_ns, sample_rate, amplitude);
    render_bit(audio, 1, base_ticks, time_unit_ns, sample_rate, amplitude);
}

static double bit_cost(
    const U64List *deltas,
    uint64_t index,
    int bit,
    double base_ticks
) {
    double sum = 0.0;
    int count = bit ? 4 : 2;
    double target = bit ? base_ticks : base_ticks * 2.0;
    int i;

    if (index + (uint64_t)count > deltas->count) {
        return 999999.0;
    }

    for (i = 0; i < count; ++i) {
        sum += rel_error_double((double)deltas->items[index + i], target);
    }

    return sum / (double)count;
}

static void update_base_from_bit(
    const U64List *deltas,
    uint64_t index,
    int bit,
    double *base_ticks
) {
    int count = bit ? 4 : 2;
    long double sum = 0.0L;
    int i;
    double measured_base;

    if (index + (uint64_t)count > deltas->count) {
        return;
    }

    for (i = 0; i < count; ++i) {
        sum += (long double)deltas->items[index + i];
    }

    measured_base = (double)(sum / (long double)count);

    if (!bit) {
        measured_base /= 2.0;
    }

    /*
        Conservative low-pass adaptation to follow tape drag.
    */
    *base_ticks = (*base_ticks * 0.92) + (measured_base * 0.08);
}

static int decode_expected_bit(
    const U64List *deltas,
    uint64_t *delta_index,
    int expected_bit,
    double *base_ticks,
    double weak_threshold,
    int *weak
) {
    double cost = bit_cost(deltas, *delta_index, expected_bit, *base_ticks);

    if (cost > weak_threshold) {
        *weak = 1;
    }

    update_base_from_bit(deltas, *delta_index, expected_bit, base_ticks);
    *delta_index += expected_bit ? 4 : 2;

    return expected_bit;
}

static int decode_data_bit(
    const U64List *deltas,
    uint64_t *delta_index,
    double *base_ticks,
    double weak_threshold,
    int *weak
) {
    double cost0 = bit_cost(deltas, *delta_index, 0, *base_ticks);
    double cost1 = bit_cost(deltas, *delta_index, 1, *base_ticks);
    int bit = cost1 < cost0 ? 1 : 0;
    double chosen = bit ? cost1 : cost0;

    if (chosen > weak_threshold) {
        *weak = 1;
    }

    update_base_from_bit(deltas, *delta_index, bit, base_ticks);
    *delta_index += bit ? 4 : 2;

    return bit;
}

static uint8_t adaptive_decode_byte(
    const U64List *deltas,
    uint64_t *delta_index,
    double *base_ticks,
    double weak_threshold,
    int *weak_frame
) {
    uint8_t value = 0;
    int i;
    int weak = 0;

    /*
        Force known frame grammar:
            start = 0
            stops = 1,1
        This is the key difference from passive parsing.
    */
    decode_expected_bit(deltas, delta_index, 0, base_ticks, weak_threshold, &weak);

    for (i = 0; i < 8; ++i) {
        int bit = decode_data_bit(deltas, delta_index, base_ticks, weak_threshold, &weak);

        if (bit) {
            value |= (uint8_t)(1u << i);
        }
    }

    decode_expected_bit(deltas, delta_index, 1, base_ticks, weak_threshold, &weak);
    decode_expected_bit(deltas, delta_index, 1, base_ticks, weak_threshold, &weak);

    *weak_frame = weak;

    return value;
}

static uint8_t parity_excluding_key(const ByteBuffer *bytes, uint64_t parity_index) {
    uint32_t sum = 0;
    uint64_t i;

    for (i = 1; i < parity_index; ++i) {
        sum += bytes->data[i];
    }

    return (uint8_t)((0u - sum) & 0xffu);
}

static uint16_t be16(uint8_t upper, uint8_t lower) {
    return (uint16_t)(((uint16_t)upper << 8) | lower);
}

static uint64_t expected_bytes_for_block(uint8_t key, const ExpectedState *state) {
    if (key == SC3K_KEY_BASIC_HEADER) {
        return 22;
    }

    if (key == SC3K_KEY_MACHINE_HEADER) {
        return 24;
    }

    if ((key == SC3K_KEY_BASIC_DATA || key == SC3K_KEY_MACHINE_DATA) && state->have_expected_length) {
        return 1ULL + (uint64_t)state->expected_length + 1ULL + 2ULL;
    }

    /*
        Fallback if no expected length is available.
    */
    return 0;
}

static void write_report_header(FILE *report, const char *input, const char *output) {
    fprintf(report, "SC-3000 Adaptive Repair Report\n");
    fprintf(report, "============================================================\n");
    fprintf(report, "Input WAV : %s\n", input);
    fprintf(report, "Output WAV: %s\n\n", output);
}

static void print_hex(FILE *report, uint8_t v) {
    static const char h[] = "0123456789ABCDEF";
    fputc(h[(v >> 4) & 15], report);
    fputc(h[v & 15], report);
}

static void analyze_decoded_block_metadata(
    FILE *report,
    ByteBuffer *bytes,
    ExpectedState *state
) {
    uint8_t key;

    if (bytes->count == 0) {
        fprintf(report, "    No bytes decoded.\n");
        return;
    }

    key = bytes->data[0];

    fprintf(report, "    Key: ");
    print_hex(report, key);
    fprintf(report, "H\n");

    if (key == SC3K_KEY_BASIC_HEADER && bytes->count >= 22) {
        uint16_t length = be16(bytes->data[17], bytes->data[18]);
        uint8_t expected_parity = parity_excluding_key(bytes, 19);

        fprintf(report, "    Type: BASIC HEADER\n");
        fprintf(report, "    Program length: %u\n", (unsigned int)length);
        fprintf(report, "    Parity: actual=");
        print_hex(report, bytes->data[19]);
        fprintf(report, " expected=");
        print_hex(report, expected_parity);
        fprintf(report, " %s\n", bytes->data[19] == expected_parity ? "OK" : "FAIL");

        state->have_expected_length = 1;
        state->expected_length = length;
        state->expected_machine = 0;

    } else if (key == SC3K_KEY_MACHINE_HEADER && bytes->count >= 24) {
        uint16_t length = be16(bytes->data[17], bytes->data[18]);
        uint16_t start = be16(bytes->data[19], bytes->data[20]);
        uint8_t expected_parity = parity_excluding_key(bytes, 21);

        fprintf(report, "    Type: MACHINE HEADER\n");
        fprintf(report, "    Program length: %u\n", (unsigned int)length);
        fprintf(report, "    Start address: %u\n", (unsigned int)start);
        fprintf(report, "    Parity: actual=");
        print_hex(report, bytes->data[21]);
        fprintf(report, " expected=");
        print_hex(report, expected_parity);
        fprintf(report, " %s\n", bytes->data[21] == expected_parity ? "OK" : "FAIL");

        state->have_expected_length = 1;
        state->expected_length = length;
        state->expected_machine = 1;

    } else if ((key == SC3K_KEY_BASIC_DATA || key == SC3K_KEY_MACHINE_DATA) && bytes->count >= 4) {
        uint64_t data_len = bytes->count - 4;
        uint64_t parity_index = 1 + data_len;
        uint8_t expected_parity = parity_excluding_key(bytes, parity_index);

        fprintf(report, "    Type: %s DATA\n", key == SC3K_KEY_BASIC_DATA ? "BASIC" : "MACHINE");
        fprintf(report, "    Data bytes decoded: ");
        print_u64(report, data_len);
        fprintf(report, "\n");

        if (state->have_expected_length) {
            fprintf(report, "    Expected data bytes: %u\n", (unsigned int)state->expected_length);
            if (data_len == state->expected_length) {
                fprintf(report, "    Length check: OK\n");
            } else if (data_len < state->expected_length) {
                fprintf(report, "    Length check: SHORT by ");
                print_u64(report, (uint64_t)state->expected_length - data_len);
                fprintf(report, "\n");
            } else {
                fprintf(report, "    Length check: LONG by ");
                print_u64(report, data_len - (uint64_t)state->expected_length);
                fprintf(report, "\n");
            }
        }

        fprintf(report, "    Parity: actual=");
        print_hex(report, bytes->data[parity_index]);
        fprintf(report, " expected=");
        print_hex(report, expected_parity);
        fprintf(report, " %s\n", bytes->data[parity_index] == expected_parity ? "OK" : "FAIL");
    }
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_wav;
    const char *report_path;

    uint64_t time_unit_ns = 4000;
    double edge_threshold = 0.35;
    uint64_t min_delta_ticks = 1;
    double gain = 8.0;
    double clip_level = 1.0;
    double seed_short = 52.0;
    double leader_tolerance = 0.35;
    double weak_threshold = 0.45;

    WavData wav;
    ConditioningStats conditioning;
    U64List edges;
    U64List deltas;
    AudioBuffer audio;
    ExpectedState state;
    FILE *report;

    uint64_t scan_delta = 0;
    uint64_t previous_output_tick = 0;
    uint64_t block_number = 0;
    int16_t amplitude = 22000;

    if (argc < 4) {
        printf("Usage:\n");
        printf("  %s input.wav output_repaired.wav report.txt [time_unit_ns] [edge_threshold] [gain] [seed_short_ticks]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s SpeechLoader.wav SpeechLoader_adaptive.wav SpeechLoader_adaptive_report.txt 4000 0.35 8.0 52\n", argv[0]);
        return 0;
    }

    input_path = argv[1];
    output_wav = argv[2];
    report_path = argv[3];

    if (argc >= 5) time_unit_ns = (uint64_t)strtoull(argv[4], NULL, 0);
    if (argc >= 6) edge_threshold = atof(argv[5]);
    if (argc >= 7) gain = atof(argv[6]);
    if (argc >= 8) seed_short = atof(argv[7]);

    if (!wav_load_pcm16_mono_simple(input_path, &wav)) {
        return 1;
    }

    condition_signal(&wav, gain, clip_level, &conditioning);

    u64_list_init(&edges);
    u64_list_init(&deltas);

    detect_edges_schmitt(&wav, time_unit_ns, edge_threshold, min_delta_ticks, &edges);
    build_deltas(&edges, &deltas);

    audio_init(&audio);
    memset(&state, 0, sizeof(state));

    report = fopen(report_path, "wb");
    if (!report) {
        fprintf(stderr, "Cannot write report: %s\n", report_path);
        return 1;
    }

    write_report_header(report, input_path, output_wav);

    fprintf(report, "Input sample rate: %u Hz\n", wav.sample_rate);
    fprintf(report, "Edges: ");
    print_u64(report, edges.count);
    fprintf(report, "\nDeltas: ");
    print_u64(report, deltas.count);
    fprintf(report, "\n\n");

    while (scan_delta < deltas.count) {
        BlockInfo block;
        uint64_t found;
        uint64_t byte_count_target;
        uint64_t byte_i;
        uint64_t delta_index;
        double base_ticks;
        ByteBuffer decoded;
        uint64_t weak_frames = 0;

        found = find_next_leader(&edges, &deltas, scan_delta, seed_short, leader_tolerance, &block);

        if (found == UINT64_MAX) {
            break;
        }

        block_number++;

        /*
            Preserve real zero/silence gap before this leader based on absolute
            source timing.
        */
        if (block.leader_start_tick > previous_output_tick) {
            render_zero_ticks(
                &audio,
                block.leader_start_tick - previous_output_tick,
                time_unit_ns,
                wav.sample_rate
            );
        }

        /*
            Render clean leader using the locally measured short period.
        */
        for (byte_i = 0; byte_i < block.leader_bits; ++byte_i) {
            render_bit(
                &audio,
                1,
                block.short_ticks,
                time_unit_ns,
                wav.sample_rate,
                amplitude
            );
        }

        byte_buffer_init(&decoded);

        delta_index = block.data_delta_index;
        base_ticks = block.short_ticks;

        /*
            Decode first byte to know the key.
        */
        {
            int weak = 0;
            uint8_t key = adaptive_decode_byte(&deltas, &delta_index, &base_ticks, weak_threshold, &weak);
            byte_buffer_push(&decoded, key);
            block.key = key;
            if (weak) weak_frames++;
        }

        byte_count_target = expected_bytes_for_block(block.key, &state);

        if (byte_count_target == 0) {
            /*
                If unknown, read a conservative amount until another leader will
                be found later by the outer scan.  This fallback is not ideal,
                but avoids infinite parsing.
            */
            byte_count_target = 4096;
        }

        for (byte_i = 1; byte_i < byte_count_target && delta_index < deltas.count; ++byte_i) {
            int weak = 0;
            uint8_t value = adaptive_decode_byte(&deltas, &delta_index, &base_ticks, weak_threshold, &weak);
            byte_buffer_push(&decoded, value);
            if (weak) weak_frames++;
        }

        block.decoded_bytes = decoded.count;
        block.weak_frames = weak_frames;
        block.data_end_tick = edges.items[delta_index < edges.count ? delta_index : edges.count - 1];

        fprintf(report, "BLOCK ");
        print_u64(report, block_number);
        fprintf(report, "\n");
        fprintf(report, "------------------------------------------------------------\n");
        fprintf(report, "Leader start delta: ");
        print_u64(report, block.leader_start_delta);
        fprintf(report, "\nLeader half-period count: ");
        print_u64(report, block.leader_half_count);
        fprintf(report, "\nLeader bit estimate: ");
        print_u64(report, block.leader_bits);
        fprintf(report, "\nEstimated short half-period: %.4f ticks\n", block.short_ticks);
        fprintf(report, "Key decoded: ");
        print_hex(report, block.key);
        fprintf(report, "H\n");
        fprintf(report, "Target bytes: ");
        print_u64(report, byte_count_target);
        fprintf(report, "\nDecoded bytes: ");
        print_u64(report, decoded.count);
        fprintf(report, "\nWeak frames: ");
        print_u64(report, weak_frames);
        fprintf(report, "\n");

        analyze_decoded_block_metadata(report, &decoded, &state);

        /*
            Render clean byte stream using the local leader-derived timing.
        */
        for (byte_i = 0; byte_i < decoded.count; ++byte_i) {
            render_byte(
                &audio,
                decoded.data[byte_i],
                block.short_ticks,
                time_unit_ns,
                wav.sample_rate,
                amplitude
            );
        }

        previous_output_tick = block.data_end_tick;
        scan_delta = delta_index;

        fprintf(report, "\n");

        byte_buffer_free(&decoded);
    }

    /*
        Preserve the final duration for easy visual comparison.
    */
    {
        uint64_t target_samples = wav.sample_count;

        if (audio.count < target_samples) {
            audio_append(&audio, 0, target_samples - audio.count);
        } else if (audio.count > target_samples) {
            audio.count = target_samples;
        }
    }

    if (!wav_write_pcm16_mono_simple(output_wav, audio.samples, audio.count, wav.sample_rate)) {
        fclose(report);
        return 1;
    }

    fprintf(report, "Adaptive repair completed.\n");
    fprintf(report, "Blocks processed: ");
    print_u64(report, block_number);
    fprintf(report, "\nOutput samples: ");
    print_u64(report, audio.count);
    fprintf(report, "\n");

    fclose(report);

    printf("SC-3000 adaptive repair completed\n");
    printf("Input : %s\n", input_path);
    printf("Output: %s\n", output_wav);
    printf("Report: %s\n", report_path);

    audio_free(&audio);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
