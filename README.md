# KeySampler

An LV2 instrument for the **ISLA S2400 DSP Card**. Play a pad from the S2400 into
KeySampler's bus once, and it records the sound and plays it chromatically from a
MIDI keyboard — with loops, a resonant filter with its own envelope, glide,
vibrato, four built-in oscillators and full control from an Edirol PCR-M30/M50/M80.

No computer is needed after the one-time install: sounds are recorded, chosen and
saved on the S2400 itself.

> Unofficial project, not affiliated with or endorsed by ISLA Instruments or Roland.

## Features

- Records the bus input (auto start on threshold, auto stop on silence); every recording gets a
  unique number (1–9999) that is saved with the S2400 project, so each project gets its own sounds back.
  The **Sound** menu reads Sine / Triangle / Square / Saw / **Sample** (this instance's own recording),
  then 1–9999 to pick or delete any recording by number
- 16 voices, ADSR, velocity, pitch bend, sustain pedal, expression
- One Shot / Gate / Loop / Ping-Pong modes; start and loop points in the S2400's own `sec : ms + smp` format
- Moog-style 24 dB/oct ladder filter (low / band / high pass) with drive, self-oscillating resonance and its own ADSR
- Portamento and legato glide; an LFO with delay to pitch, filter or volume (B4); pan
- Built-in band-limited sine, triangle, square and saw oscillators
- Laid out for the Edirol PCR-M30 **GM2 set** (L1–L3 set to CC 80–82 for Record / Save / Restore)
- **Changes** menu: Save / Restore (also L2 / L3) / Default / Delete. Controller moves change the sound only;
  Save writes them into the S2400 menu (so the project saves them), Restore goes back to the saved settings,
  Default resets everything but the sound, Delete removes the recording picked by number in Sound

## Install

1. Download `keysampler-dspcard.zip` from the [Releases](../../releases) page, or build it (below).
2. Copy the `dspcard` folder to the root of the S2400's SD card (merge with an existing one).
   The plugin must end up at `SD:/dspcard/plugins/lv2/keysampler.lv2/` (next to your other DSP plugins).
3. On the S2400 run **Sync Files** (DSP settings), then **power-cycle** the S2400 —
   plugins are scanned at start-up.
4. In the DSP Mixer load **KeySampler** on bus 1.

Everything else — recording, sample numbers, the PCR-M30 layout, the MIDI
implementation chart — is in the [manual](manual/KeySampler_Manual.pdf). A printable
template for the PCR keyboard is in [manual/](manual/) (A3 or 2 × A4).

## Build

Requirements: Python 3 and [zig](https://ziglang.org) via `pip install ziglang`
(used as the cross compiler), a native C compiler for the test build.

```sh
python3 src/gen_ttl.py   # regenerates src/keysampler.ttl from the control list
sh build.sh              # -> build/dspcard/plugins/lv2/keysampler.lv2/
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
| `tools/` | `dspfile.py` decoder and notes on the S2400 `.DSP` / `.FXP` / `.LFX` file formats |
| `third_party/lv2/` | LV2 headers (ISC licence) |

## Licence

MIT — see [LICENSE](LICENSE). The LV2 headers in `third_party/lv2` are under their own ISC licence.
