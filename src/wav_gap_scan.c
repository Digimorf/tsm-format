#include "wav_io_simple.h"
#include "tsm_v5.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
    wav_gap_scan.c

    Detects zero-like gaps in a WAV file.

    Use it to compare original and rendered WAV files.
*/

static void print_u64(uint64_t value) {
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    printf("%s", text);
}

int main(int argc, char **argv) {
    const char *path;
    double threshold = 0.03;
    double minimum_ms = 20.0;

    WavData wav;

    uint64_t minimum_samples;
    uint64_t i;

    int in_gap = 0;
    uint64_t gap_start = 0;
    uint64_t gap_count = 0;

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s input.wav [threshold] [minimum_ms]\n", argv[0]);
        return 0;
    }

    path = argv[1];

    if (argc >= 3) {
        threshold = atof(argv[2]);
    }

    if (argc >= 4) {
        minimum_ms = atof(argv[3]);
    }

    if (!wav_load_pcm16_mono_simple(path, &wav)) {
        return 1;
    }

    minimum_samples =
        (uint64_t)(
            ((double)wav.sample_rate * minimum_ms / 1000.0) + 0.5
        );

    if (minimum_samples < 1) {
        minimum_samples = 1;
    }

    printf("WAV zero-gap scan\n");
    printf("=================\n");
    printf("File: %s\n", path);
    printf("Sample rate: %u Hz\n", wav.sample_rate);

    printf("Samples: ");
    print_u64(wav.sample_count);
    printf("\n");

    printf("Threshold: %.6f\n", threshold);
    printf("Minimum gap: %.3f ms\n", minimum_ms);

    printf("\nDetected zero-like gaps\n");
    printf("-----------------------\n");

    for (i = 0; i < wav.sample_count; ++i) {
        int zero = fabs(wav.samples[i]) <= threshold;

        if (zero && !in_gap) {
            in_gap = 1;
            gap_start = i;

        } else if (!zero && in_gap) {
            uint64_t gap_end = i;
            uint64_t duration = gap_end - gap_start;

            if (duration >= minimum_samples) {
                gap_count++;

                printf("gap ");
                print_u64(gap_count);
                printf(": start=");
                print_u64(gap_start);
                printf(" end=");
                print_u64(gap_end);
                printf(" samples=");
                print_u64(duration);
                printf(" ms=%.3f\n",
                    ((double)duration * 1000.0) / (double)wav.sample_rate
                );
            }

            in_gap = 0;
        }
    }

    if (in_gap) {
        uint64_t gap_end = wav.sample_count;
        uint64_t duration = gap_end - gap_start;

        if (duration >= minimum_samples) {
            gap_count++;

            printf("gap ");
            print_u64(gap_count);
            printf(": start=");
            print_u64(gap_start);
            printf(" end=");
            print_u64(gap_end);
            printf(" samples=");
            print_u64(duration);
            printf(" ms=%.3f\n",
                ((double)duration * 1000.0) / (double)wav.sample_rate
            );
        }
    }

    printf("\nTotal zero-like gaps: ");
    print_u64(gap_count);
    printf("\n");

    wav_free_simple(&wav);

    return 0;
}
