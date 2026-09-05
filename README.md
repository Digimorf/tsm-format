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
| `SPECIFICATION.txt` | The format, the toolchain, and what each source file does. |
| `src/` | The reference implementation in C99: reading and writing TSM, converting WAV in and out, and the repair tools built on top. |
| `batch/` | Windows drag-and-drop wrappers: drop a WAV on one and it does the job. |

The C is plain C99 with no dependencies beyond the standard library. There is no
build system on purpose — `build_v5_2_compact.bat` and
`build_v5_2_plus_repair.bat` are two `cl` (or `gcc`) command lines and they are
readable enough to translate to whatever you use.

### The tools

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

TSM was built for **SE3K**, a Sega SC-3000 / SG-1000 emulator, whose virtual
datacorder FSK-demodulates a TSM the way the real SR-1000 demodulated a
cassette. It is published separately because nothing in the format is about
Sega, and a preservation format that only one emulator understands is not a
format.

What would make it one: readers in other emulators, a converter to and from the
formats that already exist, and other people's tapes proving the parts of the
model that a Sega collection cannot exercise. The specification is the whole
contract — if something in it is ambiguous, that is a bug and worth reporting.

## Licence

MIT — see `LICENSE`.
