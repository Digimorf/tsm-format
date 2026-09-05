#include "tsm_v5.h"
#include "wav_io_simple.h"
#include "indexed_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    sc3000_wav_guided_repair.c

    Sega SC-3000 guided structural repair tool.

    This is a replacement experiment for the previous adaptive repair.

    Core idea
    ---------

    Do NOT continuously change the timing reference while decoding.

    Instead:
        1. Use the leader to estimate the short half-period.
        2. Search around the end of the leader for the byte alignment that
           produces a valid key code.
        3. Decode bytes structurally:
              start bit is known to be 0
              stop bits are known to be 1
              data bits are classified from the FIRST half-period only

    Once a bit is classified:
        bit 0 -> consume 2 half-periods
        bit 1 -> consume 4 half-periods

    So, if the second half of a bit is slightly wrong, it does not destroy the
    next bit alignment.  This follows the user's proposed approach.

    This tool is a REPAIR tool, not an archival tool.
*/

#define SC3K_KEY_BASIC_HEADER   0x16
#define SC3K_KEY_BASIC_DATA     0x17
#define SC3K_KEY_MACHINE_HEADER 0x26
#define SC3K_KEY_MACHINE_DATA   0x27

#define SC3K_MIN_LEADER_BITS     3000
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
    uint64_t leader_end_delta;
    uint64_t leader_half_count;
    uint64_t leader_bits;
    double short_ticks;
} LeaderInfo;

typedef struct {
    int have_expected_length;
    uint16_t expected_length;
    int expected_machine;
} ExpectedState;

typedef struct {
    uint64_t weak_bits;
    uint64_t decoded_bytes;
    uint8_t key;
} DecodeStats;

static void print_u64(FILE *file, uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    fprintf(file, "%s", text);
}

static void print_hex(FILE *file, uint8_t v) {
    static const char h[] = "0123456789ABCDEF";
    fputc(h[(v >> 4) & 15], file);
    fputc(h[v & 15], file);
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

static double absd(double x) {
    return x < 0.0 ? -x : x;
}

static double rel_error(double measured, double target) {
    if (target <= 0.0) {
        return 999999.0;
    }

    return absd(measured - target) / target;
}

static double mean_deltas(const U64List *deltas, uint64_t start, uint64_t count) {
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

static int is_short(uint64_t d, double short_ticks, double tolerance) {
    return rel_error((double)d, short_ticks) <= tolerance;
}

static int find_next_leader(
    const U64List *deltas,
    uint64_t start_delta,
    double seed_short,
    double tolerance,
    LeaderInfo *out
) {
    uint64_t i = start_delta;

    while (i < deltas->count) {
        uint64_t run_start = i;
        uint64_t run_count = 0;
        double local = seed_short;

        while (i < deltas->count && is_short(deltas->items[i], local, tolerance)) {
            local = (local * 0.995) + ((double)deltas->items[i] * 0.005);
            run_count++;
            i++;
        }

        if (run_count >= SC3K_MIN_LEADER_BITS * 4ULL) {
            memset(out, 0, sizeof(*out));

            out->leader_start_delta = run_start;
            out->leader_end_delta = i;
            out->leader_half_count = run_count;
            out->leader_bits = run_count / 4;
            out->short_ticks = mean_deltas(deltas, run_start, run_count);

            return 1;
        }

        i++;
    }

    return 0;
}

static int is_known_key(uint8_t k) {
    return
        k == SC3K_KEY_BASIC_HEADER ||
        k == SC3K_KEY_BASIC_DATA ||
        k == SC3K_KEY_MACHINE_HEADER ||
        k == SC3K_KEY_MACHINE_DATA;
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
    double short_ticks_d,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int16_t amplitude
) {
    uint64_t short_ticks = (uint64_t)(short_ticks_d + 0.5);
    uint64_t long_ticks = (uint64_t)(short_ticks_d * 2.0 + 0.5);

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
    double short_ticks,
    uint64_t time_unit_ns,
    uint32_t sample_rate,
    int16_t amplitude
) {
    int i;

    render_bit(audio, 0, short_ticks, time_unit_ns, sample_rate, amplitude);

    for (i = 0; i < 8; ++i) {
        render_bit(audio, (value >> i) & 1, short_ticks, time_unit_ns, sample_rate, amplitude);
    }

    render_bit(audio, 1, short_ticks, time_unit_ns, sample_rate, amplitude);
    render_bit(audio, 1, short_ticks, time_unit_ns, sample_rate, amplitude);
}

static void consume_expected_bit(
    uint64_t *delta_index,
    int expected_bit
) {
    *delta_index += expected_bit ? 4 : 2;
}

static int classify_first_half_bit(
    const U64List *deltas,
    uint64_t delta_index,
    double short_ticks,
    double weak_tolerance,
    int *weak
) {
    double d;
    double e_short;
    double e_long;

    if (delta_index >= deltas->count) {
        *weak = 1;
        return 1;
    }

    d = (double)deltas->items[delta_index];
    e_short = rel_error(d, short_ticks);
    e_long = rel_error(d, short_ticks * 2.0);

    if (e_short <= e_long) {
        if (e_short > weak_tolerance) {
            *weak = 1;
        }
        return 1;
    }

    if (e_long > weak_tolerance) {
        *weak = 1;
    }

    return 0;
}

static uint8_t guided_decode_byte(
    const U64List *deltas,
    uint64_t *delta_index,
    double short_ticks,
    double weak_tolerance,
    uint64_t *weak_bits
) {
    uint8_t value = 0;
    int i;

    /*
        Known start bit.
    */
    consume_expected_bit(delta_index, 0);

    for (i = 0; i < 8; ++i) {
        int weak = 0;
        int bit = classify_first_half_bit(deltas, *delta_index, short_ticks, weak_tolerance, &weak);

        if (weak) {
            (*weak_bits)++;
        }

        if (bit) {
            value |= (uint8_t)(1u << i);
        }

        *delta_index += bit ? 4 : 2;
    }

    /*
        Known stop bits.
    */
    consume_expected_bit(delta_index, 1);
    consume_expected_bit(delta_index, 1);

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

static uint64_t expected_bytes_for_key(uint8_t key, const ExpectedState *state) {
    if (key == SC3K_KEY_BASIC_HEADER) {
        return 22;
    }

    if (key == SC3K_KEY_MACHINE_HEADER) {
        return 24;
    }

    if ((key == SC3K_KEY_BASIC_DATA || key == SC3K_KEY_MACHINE_DATA) && state->have_expected_length) {
        return 1ULL + (uint64_t)state->expected_length + 1ULL + 2ULL;
    }

    return 0;
}

static uint8_t peek_key_at_candidate(
    const U64List *deltas,
    uint64_t candidate,
    double short_ticks,
    double weak_tolerance,
    uint64_t *weak_bits
) {
    uint64_t tmp = candidate;
    uint64_t weak = 0;
    uint8_t key = guided_decode_byte(deltas, &tmp, short_ticks, weak_tolerance, &weak);

    *weak_bits = weak;
    return key;
}

static uint64_t choose_data_start(
    FILE *report,
    const U64List *deltas,
    uint64_t leader_end_delta,
    double short_ticks,
    double weak_tolerance,
    uint8_t *out_key
) {
    int shift;
    uint64_t best_candidate = leader_end_delta;
    uint8_t best_key = 0xff;
    uint64_t best_score = UINT64_MAX;

    /*
        The leader end may be off by a few half-periods because the damaged
        waveform can contain one extra/missing short pulse at the boundary.
        Try nearby offsets and choose the one that gives a known key.
    */
    for (shift = -12; shift <= 12; ++shift) {
        uint64_t candidate;
        uint64_t weak = 0;
        uint8_t key;
        uint64_t score;

        if (shift < 0 && leader_end_delta < (uint64_t)(-shift)) {
            continue;
        }

        candidate = (uint64_t)((int64_t)leader_end_delta + shift);

        if (candidate >= deltas->count) {
            continue;
        }

        key = peek_key_at_candidate(deltas, candidate, short_ticks, weak_tolerance, &weak);

        score = weak * 1000ULL + (uint64_t)(shift < 0 ? -shift : shift);

        if (is_known_key(key)) {
            score -= 500;
        }

        fprintf(report, "        candidate shift %+d -> key ", shift);
        print_hex(report, key);
        fprintf(report, "H weak_bits=");
        print_u64(report, weak);
        fprintf(report, "\n");

        if (score < best_score) {
            best_score = score;
            best_candidate = candidate;
            best_key = key;
        }

        if (is_known_key(key) && weak == 0) {
            best_candidate = candidate;
            best_key = key;
            break;
        }
    }

    *out_key = best_key;
    return best_candidate;
}

static void update_state_from_block(FILE *report, const ByteBuffer *bytes, ExpectedState *state) {
    uint8_t key;

    if (bytes->count == 0) {
        return;
    }

    key = bytes->data[0];

    if (key == SC3K_KEY_BASIC_HEADER && bytes->count >= 22) {
        uint16_t length = be16(bytes->data[17], bytes->data[18]);
        uint8_t expected_parity = parity_excluding_key(bytes, 19);

        fprintf(report, "    BASIC HEADER length=%u parity actual=", (unsigned int)length);
        print_hex(report, bytes->data[19]);
        fprintf(report, " expected=");
        print_hex(report, expected_parity);
        fprintf(report, " %s\n", bytes->data[19] == expected_parity ? "OK" : "FAIL");

        state->have_expected_length = 1;
        state->expected_length = length;
        state->expected_machine = 0;

    } else if (key == SC3K_KEY_MACHINE_HEADER && bytes->count >= 24) {
        uint16_t length = be16(bytes->data[17], bytes->data[18]);
        uint8_t expected_parity = parity_excluding_key(bytes, 21);

        fprintf(report, "    MACHINE HEADER length=%u parity actual=", (unsigned int)length);
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

        fprintf(report, "    DATA bytes=");
        print_u64(report, data_len);

        if (state->have_expected_length) {
            fprintf(report, " expected=%u", (unsigned int)state->expected_length);
        }

        fprintf(report, " parity actual=");
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
    double weak_tolerance = 0.45;

    WavData wav;
    ConditioningStats conditioning;
    U64List edges;
    U64List deltas;
    AudioBuffer audio;
    ExpectedState state;
    FILE *report;

    uint64_t scan_delta = 0;
    uint64_t previous_tick = 0;
    uint64_t block_number = 0;
    int16_t amplitude = 22000;

    if (argc < 4) {
        printf("Usage:\n");
        printf("  %s input.wav output_repaired.wav report.txt [time_unit_ns] [edge_threshold] [gain] [seed_short_ticks]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s SpeechLoader.wav SpeechLoader_guided.wav SpeechLoader_guided_report.txt 4000 0.35 8.0 52\n", argv[0]);
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

    fprintf(report, "SC-3000 Guided Structural Repair Report\n");
    fprintf(report, "============================================================\n");
    fprintf(report, "Input : %s\n", input_path);
    fprintf(report, "Output: %s\n\n", output_wav);
    fprintf(report, "Edges: ");
    print_u64(report, edges.count);
    fprintf(report, "\nDeltas: ");
    print_u64(report, deltas.count);
    fprintf(report, "\n\n");

    while (scan_delta < deltas.count) {
        LeaderInfo leader;
        uint64_t data_start;
        uint8_t key_at_start = 0xff;
        uint64_t target_bytes;
        uint64_t delta_index;
        ByteBuffer decoded;
        uint64_t i;
        uint64_t weak_bits = 0;

        if (!find_next_leader(&deltas, scan_delta, seed_short, leader_tolerance, &leader)) {
            break;
        }

        block_number++;

        fprintf(report, "BLOCK ");
        print_u64(report, block_number);
        fprintf(report, "\n------------------------------------------------------------\n");
        fprintf(report, "Leader start delta: ");
        print_u64(report, leader.leader_start_delta);
        fprintf(report, "\nLeader end delta: ");
        print_u64(report, leader.leader_end_delta);
        fprintf(report, "\nLeader half count: ");
        print_u64(report, leader.leader_half_count);
        fprintf(report, "\nLeader bits approx: ");
        print_u64(report, leader.leader_bits);
        fprintf(report, "\nEstimated short half-period: %.4f ticks\n", leader.short_ticks);
        fprintf(report, "Alignment candidates:\n");

        data_start =
            choose_data_start(
                report,
                &deltas,
                leader.leader_end_delta,
                leader.short_ticks,
                weak_tolerance,
                &key_at_start
            );

        fprintf(report, "Chosen data start delta: ");
        print_u64(report, data_start);
        fprintf(report, "\nChosen key preview: ");
        print_hex(report, key_at_start);
        fprintf(report, "H\n");

        if (edges.items[leader.leader_start_delta] > previous_tick) {
            render_zero_ticks(
                &audio,
                edges.items[leader.leader_start_delta] - previous_tick,
                time_unit_ns,
                wav.sample_rate
            );
        }

        /*
            Render a clean leader using nominal/observed leader bit count.
        */
        for (i = 0; i < leader.leader_bits; ++i) {
            render_bit(
                &audio,
                1,
                leader.short_ticks,
                time_unit_ns,
                wav.sample_rate,
                amplitude
            );
        }

        byte_buffer_init(&decoded);

        delta_index = data_start;

        /*
            Decode key first.
        */
        {
            uint64_t wb = 0;
            uint8_t key = guided_decode_byte(
                &deltas,
                &delta_index,
                leader.short_ticks,
                weak_tolerance,
                &wb
            );

            byte_buffer_push(&decoded, key);
            weak_bits += wb;
        }

        target_bytes = expected_bytes_for_key(decoded.data[0], &state);

        if (target_bytes == 0) {
            /*
                Unknown key: do not try to synthesize a huge damaged block.
            */
            target_bytes = 64;
        }

        for (i = 1; i < target_bytes && delta_index < deltas.count; ++i) {
            uint64_t wb = 0;
            uint8_t value = guided_decode_byte(
                &deltas,
                &delta_index,
                leader.short_ticks,
                weak_tolerance,
                &wb
            );

            byte_buffer_push(&decoded, value);
            weak_bits += wb;
        }

        fprintf(report, "Target bytes: ");
        print_u64(report, target_bytes);
        fprintf(report, "\nDecoded bytes: ");
        print_u64(report, decoded.count);
        fprintf(report, "\nWeak data bits: ");
        print_u64(report, weak_bits);
        fprintf(report, "\nFinal delta index: ");
        print_u64(report, delta_index);
        fprintf(report, "\n");

        update_state_from_block(report, &decoded, &state);

        for (i = 0; i < decoded.count; ++i) {
            render_byte(
                &audio,
                decoded.data[i],
                leader.short_ticks,
                time_unit_ns,
                wav.sample_rate,
                amplitude
            );
        }

        previous_tick = edges.items[delta_index < edges.count ? delta_index : edges.count - 1];
        scan_delta = delta_index;

        fprintf(report, "\n");

        byte_buffer_free(&decoded);
    }

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

    fprintf(report, "Guided repair completed.\n");
    fprintf(report, "Blocks processed: ");
    print_u64(report, block_number);
    fprintf(report, "\nOutput samples: ");
    print_u64(report, audio.count);
    fprintf(report, "\n");

    fclose(report);

    printf("SC-3000 guided repair completed\n");
    printf("Input : %s\n", input_path);
    printf("Output: %s\n", output_wav);
    printf("Report: %s\n", report_path);

    audio_free(&audio);
    u64_list_free(&edges);
    u64_list_free(&deltas);
    wav_free_simple(&wav);

    return 0;
}
