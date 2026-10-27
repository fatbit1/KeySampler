#!/usr/bin/env python3
"""Decode ISLA S2400 DSP Card files: .DSP (DSP mixer config), .LFX (Live FX config)
and .FXP (plugin preset). Formats worked out from files saved by an S2400 with DSP
Card firmware 3.1.12; tag meanings marked '?' are guesses.

.DSP / .LFX: a flat list of records  <type u8> <tag u16 LE> <value>
    type 1 = u32 LE, type 4 = float32 LE, type 3 = u32 length + bytes (string, NUL-terminated)
.FXP: standard VST2 "CcnK"/"FxCk" preset, but little-endian, values normalized 0..1.
"""
import struct, sys

DSP_TAGS = {
    0x00: "file version", 0x02: "bus count", 0x18: "stereo pairs?",
    0x03: "BUS", 0x06: "level", 0x04: "mute?", 0x1a: "pan (127 = centre)?",
    0x09: "send A amount?", 0x07: "send A type?", 0x0a: "send B amount?", 0x08: "send B type?",
    0x19: "send to main off?", 0x0b: "plugin count",
    0x0c: "PLUGIN slot", 0x0d: "plugin id (hash)", 0x0e: "plugin bypassed?",
    0x0f: "param index", 0x10: "param value (normalized)",
    0x16: "name index", 0x17: "bus name",
}
BUS_NAMES = ["1", "2", "3", "4", "5", "6", "7", "8", "1+2", "3+4", "5+6", "7+8", "Return A", "Return B", "Main"]

def records(data):
    i = 0
    while i + 3 <= len(data):
        t = data[i]; tag = struct.unpack_from("<H", data, i + 1)[0]; i += 3
        if t == 1: v = struct.unpack_from("<I", data, i)[0]; i += 4
        elif t == 4: v = struct.unpack_from("<f", data, i)[0]; i += 4
        elif t == 3:
            n = struct.unpack_from("<I", data, i)[0]; i += 4
            v = data[i:i + n].rstrip(b"\0").decode("latin1"); i += n
        else:
            yield ("?", tag, data[i - 3:].hex()); return
        yield (t, tag, v)

def read_ttl_names(ttl_path):
    import re
    if not ttl_path: return None
    ttl = open(ttl_path).read()
    ports = re.findall(r'lv2:index (\d+) ;\s+lv2:symbol "([^"]+)" ;\s+lv2:name "([^"]+)" ;\s+lv2:default ([-\d.]+) ;\s+lv2:minimum ([-\d.]+) ;\s+lv2:maximum ([-\d.]+)', ttl)
    return [(n, float(lo), float(hi)) for i, sym, n, d, lo, hi in ports]

def dump_dsp(path, names=None, plugin_id=None):
    data = open(path, "rb").read()
    print(f"== {path} ({len(data)} bytes)")
    bus = None; pidx = None
    for t, tag, v in records(data):
        if t == "?": print("  undecoded tail:", v); break
        name = DSP_TAGS.get(tag, f"tag 0x{tag:02x}")
        if tag == 0x03: bus = v; print(f"  -- bus {BUS_NAMES[v] if v < len(BUS_NAMES) else v}"); continue
        if tag == 0x0d: plugin_id = v; print(f"     plugin id 0x{v:08x}"); continue
        if tag == 0x0f: pidx = v; continue
        if tag == 0x10:
            label = ""
            if names and pidx < len(names):
                n, lo, hi = names[pidx]; label = f"{n:18s} = {lo + v * (hi - lo):g}"
            print(f"       param {pidx:2d}: {v:.4f}  {label}"); continue
        print(f"     {name}: {v}")

def dump_fxp(path, names=None):
    d = open(path, "rb").read()
    magic, size, kind, ver, fxid, fxver, n = struct.unpack_from("<4sI4sIIII", d, 0)
    prg = d[28:56].rstrip(b"\0").decode("latin1")
    print(f"== {path}: {magic.decode()} {kind.decode()} v{ver} plugin id 0x{fxid:08x} fxVersion {fxver} params {n} name '{prg}'")
    vals = struct.unpack_from(f"<{n}f", d, 56)
    for i, v in enumerate(vals):
        label = ""
        if names and i < len(names):
            nm, lo, hi = names[i]; label = f"{nm:18s} = {lo + v * (hi - lo):g}"
        print(f"   {i:2d}: {v:.4f}  {label}")

if __name__ == "__main__":
    args = sys.argv[1:]
    ttl = None
    if args and args[0] == "--ttl": ttl = args[1]; args = args[2:]
    names = read_ttl_names(ttl)
    for p in args:
        (dump_fxp if p.upper().endswith(".FXP") else dump_dsp)(p, names)
