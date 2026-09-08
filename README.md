# TSM — Temporal Signal Medium

A format for storing what a tape *did*, not what it meant.

A cassette carries a two-state signal: the level is high, then low, then high
again. Every format built to preserve those tapes has had to decide how much of
the loader to understand — `.cas` understands the bytes, `.tap` the bits, `.tzx`
the blocks — and each of those decisions eventually meets a tape that does not
fit it. TSM decides nothing. It records **when the signal changed**, to a tick,
and leaves the understanding to whatever plays it back.

That makes it independent of the machine. The same file describes a standard KCS
tape from an SC-3000 or an MSX, a ZX Spectrum ROM loader, a turbo loader nobody
documented, a protection scheme built on timing, or a tape that changes format
half way through — because none of that is the format's business.

**Current version: v5.2 "Compact Escape"** — magic `TSM5`, version 5.

## How it is put together

A tape is a sequence of **regions**, and there are three kinds:

| Region | Value | What it holds |
|---|---|---|
| `ZERO_SILENCE` | 4 | A true gap: no signal at all. |
| `SILENCE` / hold | 0 | The level stays where it was. No edges, a few bytes. |
| `INDEXED_DELTA` | 3 | The signal itself, as the intervals between its edges. |

A file may also carry a **label section**, appended after the payloads and
found from the end: what the programs on the tape are called, which system
wrote each region, where the recording came from, what has to be typed to load
it. It is optional and invisible to a reader that does not want it — nothing
before it moves, so a TSM with labels is a TSM without them plus bytes at the
end.

Long pilot tones and the silences between blocks cost almost nothing; the data
blocks are a compact nibble stream with an escape for the intervals that do not
fit the table. The tick is configurable and defaults to 4 µs, which is finer
than any tape loader cares about and far cheaper than storing samples.

An 80-byte header, a table of 104-byte region entries, then the payload. The
whole thing is in [`SPECIFICATION.txt`](SPECIFICATION.txt) — header fields,
region fields, the delta encoding, and the reasoning behind each.

## What is here

| Folder | What it is |
|---|---|
| `SPECIFICATION.txt` | The format itself: every field, the encoding, and the reasoning behind each. |
| `src/` | The reference implementation in C99: reading and writing TSM, converting WAV in and out, and the repair tools built on top. |
| `batch/` | Windows drag-and-drop wrappers: drop a WAV on one and it does the job. |
| `web/` | The same sources built to WebAssembly, so a page can read a TSM and play it without a server. |

## Building it

```
make            # eleven tools into bin/
make check      # build them, then run each one
```

C99, nothing beyond the standard library and `libm`, clean under `-Wall
-Wextra`. On Windows without `make`, `batch/build_v5_2_compact.bat` does the
same with plain `gcc` lines.

## In a browser

```
web/build.sh            # needs emcc on PATH; writes web/tsm.js and web/tsm.wasm
```

Two entry points: `tsm_web_describe()` returns the regions and labels as JSON,
`tsm_web_render()` returns the signal as float samples ready for Web Audio.
Both are thin: the renderer is `tsm2wav_v5.c` exactly as it ships, with only
its entry point renamed, so the page and `bin/tsm2wav_v5` cannot drift apart.
On a 106-second tape the two agree sample for sample, and the render takes
about a third of a second.

## A tape, there and back

```
bin/wav2tsm_indexed_v5  tape.wav  tape.tsm  4000 16 0.35 1 8.0
bin/tsm_v5_indexed_audit          tape.tsm
bin/tsm2wav_v5                    tape.tsm  again.wav 44100
```

On a Sega SC-3000 recording of 600 known bytes: a 910 KB WAV becomes an 18 KB
TSM — fifty times smaller — and renders back to a WAV of exactly the same
length, which decodes to the same 600 bytes across the same 34,793 edges. The
saving is not in throwing anything away. It is in not storing the shape of a
sine wave nobody needs.

## Every tool

| Tool | What it does |
|---|---|
| `wav2tsm_indexed_v5` | A WAV recording of a tape becomes a TSM. |
| `tsm2wav_v5` | And back again, for listening or for a real machine. |
| `wav2bits_kcs` | KCS tapes straight to a bitstream, skipping TSM. |
| `sc3000_wav_analyzer` | What is on this tape: blocks, leaders, timing. |
| `sc3000_wav_guided_repair` | Repairs an SC-3000 BASIC tape using what the block says about itself — its header's length and its parity. |
| `sc3000_wav_adaptive_repair` | Repairs by following the tape's own drifting timing. |
| `wav_repair_kcs`, `wav_repair_kcs_framed` | The same for KCS generally, by cell and by frame. |
| `tsm_v5_indexed_audit` | Checks a TSM against itself: regions, deltas, totals. |
| `wav_gap_scan` | Finds the silences, which is where a tape divides. |
| `wav_tsm_advisor_v5_indexed` | Suggests the encoding parameters for a given recording. |

## Why the edges and not the samples

Storing the samples is honest and enormous: a 90-minute cassette at 48 kHz is
half a gigabyte, and almost all of it describes the shape of a sine wave nobody
needs. Storing the decoded bytes is small and lossy in the way that matters —
it throws away the timing, and the timing *is* the protection scheme, the turbo
loader, and the evidence when a tape does not load.

Edges keep what a tape actually did. A TSM of that same cassette is a couple of
hundred kilobytes, and a machine reading it back through its own demodulator
behaves exactly as it did with the tape: the loader is not bypassed, it is fed.

## Where it came from, and where it is going

TSM was written by Francesco De Simone while building an emulator that needed a
tape format able to hold anything a cassette could carry. That emulator is a
separate project and is not required here: nothing in this repository depends on
it, and nothing in the format is about any one machine. It is worth a mention
only because the format has been implemented twice, independently, from the
document in `SPECIFICATION.txt` — which is the test a specification has to
pass.

What would make it a standard rather than one person's format: readers in other
emulators, converters to and from the formats that already exist, and other
people's tapes exercising the parts of the model that a single collection
cannot. The specification is the whole contract. If something in it is
ambiguous, that is a bug and worth reporting.

## Licence

Two licences, because there are two different things here.

**The specification** (`SPECIFICATION.txt`) is under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Implement TSM in any
language, for any purpose, commercial or not, without asking and without using
a line of this code. Name the format as TSM, by Francesco De Simone. A format
nobody may implement freely is not a format.

**The reference implementation** (`src/`, `batch/`, `Makefile`) is under the
[Apache License 2.0](LICENSE). Use it, change it, ship it inside a product,
sell it. What Apache asks in return is its section 4(d): the attribution in
[`NOTICE`](NOTICE) travels with the work, so wherever the code ends up, where
it came from goes with it.

Copyright (c) 2026 Francesco De Simone.
