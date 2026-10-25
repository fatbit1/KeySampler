# KeySampler

An LV2 instrument for the **ISLA S2400 DSP Card**. Play a pad from the S2400 into
KeySampler's bus once, and it records the sound and plays it chromatically from a
MIDI keyboard — with loops, a resonant filter with its own envelope, glide,
vibrato, four built-in oscillators and full control from an Edirol PCR-M30/M50/M80.

No computer is needed after the one-time install: sounds are recorded, chosen and
saved on the S2400 itself.

> Unofficial project, not affiliated with or endorsed by ISLA Instruments or Roland.

## Features

- Records the bus input into 64 slots × 99 banks (auto start on threshold, auto stop on silence)
- 16 voices, ADSR, velocity, pitch bend, sustain pedal, expression
- One Shot / Gate / Loop modes; start and loop points in the S2400's own `sec : ms + smp` format
- Low / band / high pass filter with resonance and a switchable filter ADSR
- Portamento and legato glide, delayed vibrato, pan
- Slots 61–64 are band-limited sine, triangle, square and saw oscillators
- MIDI CC map following the PCR-M30 **GM2 set**, plus CC Learn for any other controller
- **B5 Save / B6 Restore**: controller moves change the sound only; B5 writes them into the
  S2400 menu (so the project saves them), B6 goes back to the menu values

## Install

1. Download `keysampler-dspcard.zip` from the [Releases](../../releases) page, or build it (below).
2. Copy the `dspcard` folder to the root of the S2400's SD card (merge with an existing one).
   The plugin must end up at `SD:/dspcard/lv2/keysampler.lv2/`.
3. On the S2400 run **Sync Files** (DSP settings), then **power-cycle** the S2400 —
   plugins are scanned at start-up.
4. In the DSP Mixer load **KeySampler** on bus 1.

Everything else — recording, banks, the PCR-M30 layout, CC Learn, the MIDI
implementation chart — is in the [manual](manual/KeySampler_Manual.pdf). A printable
template for the PCR keyboard is in [manual/](manual/) (A3 or 2 × A4).

## Build

Requirements: Python 3 and [zig](https://ziglang.org) via `pip install ziglang`
(used as the cross compiler), a native C compiler for the test build.

```sh
python3 src/gen_ttl.py   # regenerates src/keysampler.ttl from the control list
sh build.sh              # -> build/dspcard/lv2/keysampler.lv2/
```

zig targets glibc 2.17 so the library loads on the DSP Card (glibc 2.37). Distribution
cross-gccs usually link newer symbols and produce a plugin the card silently ignores;
`build.sh` warns if that happens.

## Layout

| Path | Contents |
| --- | --- |
| `src/keysampler.c` | the plugin (real-time voice engine, recorder, MIDI/CC handling) |
| `src/grpc_lite.c` | tiny HTTP/2 gRPC client used by Save to Menu to talk to the card's sushi host |
| `src/gen_ttl.py` | generates the LV2 port description; its order is the S2400 menu order (first 32 shown) |
| `test/` | lilv test hosts, sample WAVs and a stand-in for sushi's gRPC API — see [test/README.md](test/README.md) |
| `manual/` | manual and PCR template sources (HTML → PDF via Playwright) and the rendered PDFs |
| `third_party/lv2/` | LV2 headers (ISC licence) |

## Licence

MIT — see [LICENSE](LICENSE). The LV2 headers in `third_party/lv2` are under their own ISC licence.
