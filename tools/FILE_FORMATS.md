# S2400 DSP Card file formats

Worked out from files saved by an S2400 with DSP Card firmware 3.1.12 (October 2026).
Tag names marked **?** are educated guesses from comparing projects.
`tools/dspfile.py` decodes all three formats:

```sh
python3 tools/dspfile.py --ttl src/keysampler.ttl Project012.DSP KeySampler.FXP
```

## `.DSP` — DSP mixer configuration (saved with every project, or with Save Config)

A flat list of records, little-endian:

| Byte(s) | Meaning |
| --- | --- |
| 1 | type: `1` = u32, `4` = float32, `3` = string (u32 length + bytes, NUL-terminated) |
| 2 | tag (u16) |
| … | value |

Records appear in this order:

| Tag | Type | Meaning |
| --- | --- | --- |
| `0x00` | u32 | file version (`0x00040002`) |
| `0x02` | u32 | 15 = number of buses that follow? (one file had 11) |
| `0x18` | u32 | ? (always 0 so far) |
| `0x03` | u32 | **start of a bus**: 0–7 = buses 1–8, 8–11 = pairs 1+2…7+8, 12/13 = returns A/B, 14 = main |
| `0x06` | u32 | level, 0–255 (255 = default) |
| `0x04` | u32 | mute? |
| `0x1a` | u32 | pan, 0–254, 127 = centre |
| `0x09` `0x07` | u32 | send A amount / type? |
| `0x0a` `0x08` | u32 | send B amount / type? |
| `0x19` | u32 | "send to main" off? |
| `0x0b` | u32 | number of plugins on this bus |
| `0x0c` | u32 | **start of a plugin**: slot index |
| `0x0d` | u32 | plugin ID (hash computed by ISLA's sushi; `GetPluginInfo` lists them) |
| `0x0e` | u32 | plugin disabled? |
| `0x0f` | u32 | parameter index |
| `0x10` | float | parameter value, **normalized 0–1** |
| `0x16` / `0x17` | u32 / string | bus name index / bus name ("One" … "Eight") |

Buses 1–8 carry the full set of fields; pairs, returns and main only level and plugins.

**Every** parameter the plugin reports is stored — not only the 32 the S2400 shows —
by **index** and **normalized**. So for a plugin to load old projects correctly its
parameter order and ranges must never change; new parameters go at the end.

KeySampler's ID is `0xdcef1754` (it depends on the plugin, not on its version).
Some built-in IDs: mda Delay `0xcb9272b2`, mda DubDelay `0x3a79f982`, mda Dynamics
`0x2fa5cda5`, mda ePiano `0x1f8d6b16`, mda SubSynth `0x9240413b`, Airwindows Reverb
`0x5291fa4c`, Airwindows FireAmp `0x4da63885`.

## `.FXP` — plugin preset (Save Preset)

The standard VST2 `CcnK`/`FxCk` program format, but **little-endian**:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | `CcnK` |
| 4 | 4 | byte size of the rest |
| 8 | 4 | `FxCk` (parameter list) |
| 12 | 4 | version (1) |
| 16 | 4 | plugin ID (same hash as in `.DSP`) |
| 20 | 4 | plugin version (1) |
| 24 | 4 | number of parameters |
| 28 | 28 | program name |
| 56 | 4 × n | parameter values, float32, normalized 0–1 |

## `.LFX` (Live FX) and `S2400.SET` (global settings)

Same record format as `.DSP`. A `.LFX` holds a version (`0x00050001`) and, for each of
the 8 Live FX slots, tag `0x01` = slot and `0x02` = algorithm.
