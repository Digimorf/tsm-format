/*
 * TSM - Temporal Signal Medium
 * The reference tools, made callable from a web page.
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

/*
 * This adds nothing to the format and decodes nothing of its own. It writes
 * the file into Emscripten's in-memory filesystem and calls the same
 * tsm2wav_v5 that ships in bin/, unchanged, compiled with -Dmain=tsm2wav_main.
 * What runs in the browser is therefore the published reference tool and not a
 * second implementation that could drift from it - which is the whole reason
 * the player is built from this repository rather than from an emulator.
 *
 * Two entry points:
 *
 *   tsm_web_describe(data, size)
 *       What is on the tape, as JSON: the header, every region, and the label
 *       section of 2.5 if the file carries one.
 *
 *   tsm_web_render(data, size, sample_rate, amplitude, &count)
 *       The signal, as float samples in -1..1, which is what Web Audio takes.
 */

#include "tsm_v5.h"
#include "wav_io_simple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define TSM_WEB_API EMSCRIPTEN_KEEPALIVE
#else
#define TSM_WEB_API
#endif

/* tsm2wav_v5.c, compiled with -Dmain=tsm2wav_main. */
int tsm2wav_main(int argc, char **argv);

#define TSM_WEB_IN  "/tsm_in.tsm"
#define TSM_WEB_OUT "/tsm_out.wav"

/* ------------------------------------------------------------------------ */

/* DESCRIPTION : Put a buffer where the file-based tools can open it.
 * PARAMETERS  : path - name to give it in the in-memory filesystem
 *               data - the bytes
 *               size - how many
 * RETURNS     : 1 on success, 0 if it could not be written. */
static int tsm_web_spill(const char *path, const uint8_t *data, int size)
{
    FILE *f = fopen(path, "wb");
    size_t put;

    if (!f) {
        return 0;
    }

    put = fwrite(data, 1, (size_t)size, f);
    fclose(f);

    return put == (size_t)size;
}

/* DESCRIPTION : Silence the renderer's progress report, and let it speak
 *               again afterwards.
 *
 *               tsm2wav is a command-line tool and says what it did on its way
 *               out. That belongs in a terminal, not in a page's console once
 *               per tape, so stdout is pointed at nothing for the length of
 *               the call. stderr is left alone: if the render fails, the
 *               reason is worth having.
 * PARAMETERS  : none
 * RETURNS     : nothing. */
static void tsm_web_hush(void)
{
    fflush(stdout);
    freopen("/dev/null", "w", stdout);
} // End tsm_web_hush()

/* DESCRIPTION : Undo tsm_web_hush().
 * PARAMETERS  : none
 * RETURNS     : nothing. */
static void tsm_web_speak(void)
{
    fflush(stdout);
    freopen("/dev/stdout", "w", stdout);
} // End tsm_web_speak()

/* DESCRIPTION : Append text to a growing buffer.
 * PARAMETERS  : buf/len/cap - the buffer and where it is up to
 *               text        - what to add
 * RETURNS     : 1 on success, 0 out of memory. */
static int tsm_web_add(char **buf, size_t *len, size_t *cap, const char *text)
{
    size_t n = strlen(text);

    if (*len + n + 1 > *cap) {
        size_t want = (*cap ? *cap : 4096);
        char *bigger;

        while (want < *len + n + 1) {
            want *= 2;
        }

        bigger = (char *)realloc(*buf, want);

        if (!bigger) {
            return 0;
        }

        *buf = bigger;
        *cap = want;
    }

    memcpy(*buf + *len, text, n + 1);
    *len += n;
    return 1;
}

/* DESCRIPTION : Add a string as a JSON value, escaping what has to be.
 * PARAMETERS  : buf/len/cap - the buffer
 *               text        - the string, UTF-8
 * RETURNS     : 1 on success, 0 out of memory. */
static int tsm_web_add_json_string(char **buf, size_t *len, size_t *cap,
                                   const char *text)
{
    char one[8];

    if (!tsm_web_add(buf, len, cap, "\"")) {
        return 0;
    }

    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;

        if (c == '"' || c == '\\') {
            one[0] = '\\';
            one[1] = (char)c;
            one[2] = 0;
        } else if (c == '\n') {
            strcpy(one, "\\n");
        } else if (c == '\r') {
            strcpy(one, "\\r");
        } else if (c == '\t') {
            strcpy(one, "\\t");
        } else if (c < 0x20) {
            /* A control byte no JSON parser should have to guess at. */
            sprintf(one, "\\u%04x", c);
        } else {
            one[0] = (char)c;
            one[1] = 0;
        }

        if (!tsm_web_add(buf, len, cap, one)) {
            return 0;
        }
    }

    return tsm_web_add(buf, len, cap, "\"");
}

/* DESCRIPTION : Add a number as a JSON value. */
static int tsm_web_add_u64(char **buf, size_t *len, size_t *cap, uint64_t value)
{
    char text[32];

    tsm5_u64_to_dec(value, text, sizeof(text));
    return tsm_web_add(buf, len, cap, text);
}

/* ------------------------------------------------------------------------ */

/* DESCRIPTION : What is on this tape, as JSON.
 *
 *               The header, every region with its mode and where it sits, and
 *               the label section of specification 2.5 when the file carries
 *               one. Nothing is interpreted: a region's mode is reported, not
 *               judged, and a label's kind is passed through even when this
 *               build does not know what it means.
 *
 * PARAMETERS  : data - the whole TSM
 *               size - its length
 * RETURNS     : A malloc'd JSON string the caller frees with tsm_web_free(),
 *               or NULL. On a file that is not a TSM the JSON says so in its
 *               "error" member rather than coming back empty. */
TSM_WEB_API char *tsm_web_describe(const uint8_t *data, int size)
{
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    FILE *f;
    TSM5Header header;
    TSM5Region region;
    TSM5Label *labels = NULL;
    uint32_t label_count;
    uint32_t i;

    if (!data || size <= 0) {
        return NULL;
    }

    if (!tsm_web_spill(TSM_WEB_IN, data, size)) {
        return NULL;
    }

    f = fopen(TSM_WEB_IN, "rb");

    if (!f) {
        return NULL;
    }

    if (!tsm5_read_header(f, &header)) {
        fclose(f);
        (void)tsm_web_add(&out, &len, &cap,
                          "{\"error\":\"not a TSM v5 file\"}");
        return out;
    }

    if (!tsm_web_add(&out, &len, &cap, "{\"version\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.version)
     || !tsm_web_add(&out, &len, &cap, ",\"tick_ns\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.base_time_unit_ns)
     || !tsm_web_add(&out, &len, &cap, ",\"total_ticks\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.total_duration_ticks)
     || !tsm_web_add(&out, &len, &cap, ",\"initial_state\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.reserved0 ? 1 : 0)
     || !tsm_web_add(&out, &len, &cap, ",\"source_samples\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.reserved1)
     || !tsm_web_add(&out, &len, &cap, ",\"source_rate\":")
     || !tsm_web_add_u64(&out, &len, &cap, header.reserved2)
     || !tsm_web_add(&out, &len, &cap, ",\"regions\":[")) {
        fclose(f);
        free(out);
        return NULL;
    }

    for (i = 0; i < header.region_count; ++i) {
        long at = (long)(header.region_table_offset
                         + (uint64_t)i * header.region_entry_size);

        if (fseek(f, at, SEEK_SET) != 0 || !tsm5_read_region(f, &region)) {
            break;
        }

        if (i && !tsm_web_add(&out, &len, &cap, ",")) {
            break;
        }

        if (!tsm_web_add(&out, &len, &cap, "{\"mode\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.region_mode)
         || !tsm_web_add(&out, &len, &cap, ",\"encoding\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.encoding)
         || !tsm_web_add(&out, &len, &cap, ",\"start_ticks\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.start_ticks)
         || !tsm_web_add(&out, &len, &cap, ",\"duration_ticks\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.duration_ticks)
         || !tsm_web_add(&out, &len, &cap, ",\"tick_ns\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.time_unit_ns)
         || !tsm_web_add(&out, &len, &cap, ",\"data_size\":")
         || !tsm_web_add_u64(&out, &len, &cap, region.data_size)
         || !tsm_web_add(&out, &len, &cap, "}")) {
            break;
        }
    }

    fclose(f);

    if (!tsm_web_add(&out, &len, &cap, "],\"labels\":[")) {
        free(out);
        return NULL;
    }

    label_count = tsm5_read_labels(data, (uint64_t)size, &labels);

    for (i = 0; i < label_count; ++i) {
        if (i && !tsm_web_add(&out, &len, &cap, ",")) {
            break;
        }

        if (!tsm_web_add(&out, &len, &cap, "{\"kind\":")
         || !tsm_web_add_u64(&out, &len, &cap, labels[i].kind)
         || !tsm_web_add(&out, &len, &cap, ",\"region\":")
         || !tsm_web_add_u64(&out, &len, &cap, labels[i].region)
         || !tsm_web_add(&out, &len, &cap, ",\"from_ticks\":")
         || !tsm_web_add_u64(&out, &len, &cap, labels[i].from_ticks)
         || !tsm_web_add(&out, &len, &cap, ",\"to_ticks\":")
         || !tsm_web_add_u64(&out, &len, &cap, labels[i].to_ticks)
         || !tsm_web_add(&out, &len, &cap, ",\"text\":")
         || !tsm_web_add_json_string(&out, &len, &cap, labels[i].text)
         || !tsm_web_add(&out, &len, &cap, "}")) {
            break;
        }
    }

    tsm5_free_labels(labels, label_count);

    if (!tsm_web_add(&out, &len, &cap, "]}")) {
        free(out);
        return NULL;
    }

    remove(TSM_WEB_IN);
    return out;
}

/* DESCRIPTION : Render a TSM to 16-bit mono samples.
 *
 *               The work is done by tsm2wav_v5 exactly as it ships: the file
 *               goes into the in-memory filesystem, the tool renders a WAV
 *               beside it, and the samples are handed back. Nothing about the
 *               decoding lives here, so the page cannot drift from the
 *               reference tool.
 *
 * PARAMETERS  : data        - the whole TSM
 *               size        - its length
 *               sample_rate - what to render at, 8000 or more
 *               amplitude   - 1..32767
 *               out_count   - receives the number of samples
 * RETURNS     : A malloc'd array of float samples in -1..1, which the caller
 *               frees with tsm_web_free(), or NULL. */
TSM_WEB_API float *tsm_web_render(const uint8_t *data, int size,
                                  int sample_rate, int amplitude,
                                  int *out_count)
{
    char rate_text[32];
    char amp_text[32];
    char *argv[5];
    WavData wav;
    float *samples;
    uint64_t i;
    int rendered;

    if (out_count) {
        *out_count = 0;
    }

    if (!data || size <= 0 || sample_rate < 8000
     || amplitude < 1 || amplitude > 32767) {
        return NULL;
    }

    if (!tsm_web_spill(TSM_WEB_IN, data, size)) {
        return NULL;
    }

    sprintf(rate_text, "%d", sample_rate);
    sprintf(amp_text, "%d", amplitude);

    argv[0] = (char *)"tsm2wav";
    argv[1] = (char *)TSM_WEB_IN;
    argv[2] = (char *)TSM_WEB_OUT;
    argv[3] = rate_text;
    argv[4] = amp_text;

    tsm_web_hush();
    rendered = tsm2wav_main(5, argv);
    tsm_web_speak();

    if (rendered != 0) {
        remove(TSM_WEB_IN);
        return NULL;
    }

    if (!wav_load_pcm16_mono_simple(TSM_WEB_OUT, &wav)) {
        remove(TSM_WEB_IN);
        remove(TSM_WEB_OUT);
        return NULL;
    }

    remove(TSM_WEB_IN);
    remove(TSM_WEB_OUT);

    /* The reader hands back doubles; Web Audio wants floats, and going
     * straight from one to the other skips a needless trip through int16. */
    samples = (float *)malloc((size_t)wav.sample_count * sizeof(float));

    if (!samples) {
        wav_free_simple(&wav);
        return NULL;
    }

    for (i = 0; i < wav.sample_count; ++i) {
        samples[i] = (float)wav.samples[i];
    }

    if (out_count) {
        *out_count = (int)wav.sample_count;
    }

    wav_free_simple(&wav);
    return samples;
}

/* DESCRIPTION : Release what either of the two above handed back. */
TSM_WEB_API void tsm_web_free(void *p)
{
    free(p);
}
