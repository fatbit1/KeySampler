#!/usr/bin/env python3
"""Writes keysampler.ttl. The order of CONTROLS must match the P_* enum and
port_sym[] in keysampler.c. The S2400 shows only the first 32 controls."""
import os

URI = "https://github.com/keysampler/KeySampler"
MINOR = 41

def enum(*labels):
    return [("scale", labels)]

INT, TOG = "int", "toggle"


# (symbol, menu name, default, min, max, kind or scale labels)
CONTROLS = [
    # ---- the menu: the S2400 shows these 32 ----
    ("sample", "Sound", 0, 0, 10003, ["Sine", "Triangle", "Square", "Saw", "Sample"] + [str(i) for i in range(1, 10000)]),  # Sample = own recording; 1-9999 pick any
    ("record", "Record", 0, 0, 1, TOG),
    ("threshold", "Rec Threshold dB", -50.0, -80.0, -6.0, None),
    ("mode", "Mode", 1, 0, 3, ["One Shot", "Gate", "Loop", "Ping-Pong"]),
    ("level", "Level", 1.0, 0.0, 2.0, None),
    ("root", "Root Note", 60, 0, 127, INT),
    ("tune", "Tune", 0.0, -24.0, 24.0, None),
    ("filter", "Filter", 0, 0, 3, ["Off", "Low Pass", "Band Pass", "High Pass"]),
    ("cutoff", "Cutoff", 100.0, 0.0, 100.0, None),
    ("resonance", "Resonance", 0.0, 0.0, 1.0, None),
    ("fenv_amount", "F.Env Amount", 0.0, -100.0, 100.0, None),  # 0 = filter envelope off
    ("glide_mode", "Glide Mode", 0, 0, 2, ["Off", "Portamento", "Legato"]),
    ("glide", "Glide ms", 100.0, 0.0, 2000.0, None),
    ("ks_pan", "Pan", 0.0, -1.0, 1.0, None),  # not "pan": the DSP Card software uses that name for bus pan
    ("voices", "Voices", 8, 1, 16, INT),
    ("midi_ch", "MIDI Channel", 0, 0, 16, ["Omni"] + [str(i) for i in range(1, 17)]),
    ("velsens", "Vel Sens", 1.0, 0.0, 1.0, None),
    ("bend", "Bend Range", 2, 0, 24, INT),
    ("save_restore", "Changes", 0, 0, 4, ["---", "Save", "Restore", "Default", "Delete"]),
    ("start_sec", "Start sec", 0, 0, 59, INT),
    ("start_msec", "Start ms", 0, 0, 999, INT),
    ("start_smp", "Start smp", 0, 0, 95, INT),
    ("loop_start_sec", "Loop Start sec", 0, 0, 59, INT),
    ("loop_start_msec", "Loop Start ms", 0, 0, 999, INT),
    ("loop_start_smp", "Loop Start smp", 0, 0, 95, INT),
    ("loop_end_sec", "Loop End sec", 0, 0, 59, INT),
    ("loop_end_msec", "Loop End ms", 0, 0, 999, INT),
    ("loop_end_smp", "Loop End smp", 0, 0, 95, INT),
    ("modulation", "Modulation", 0, 0, 3, ["Volume", "Filter", "Pitch", "Pan"]),  # where R3-R5 (and S1-S4 for Filter) go; B4-B6 too
    ("lfo_rate", "LFO Rate Hz", 5.0, 0.1, 12.0, None),    # these three show / edit the LFO Modulation selects
    ("lfo_depth", "LFO Depth", 0.0, 0.0, 100.0, None),
    ("lfo_delay", "LFO Delay ms", 0.0, 0.0, 2000.0, None),
    # ---- hidden (saved with the project; PCR / defaults) ----
    ("thru", "Thru", 1, 0, 1, TOG),
    ("fenv", "Filter Env", 1, 0, 1, TOG),
    ("attack", "Attack ms", 1.0, 0.0, 2000.0, None),
    ("decay", "Decay ms", 200.0, 0.0, 5000.0, None),
    ("sustain", "Sustain", 1.0, 0.0, 1.0, None),
    ("release", "Release ms", 150.0, 0.0, 5000.0, None),
    ("fenv_attack", "F.Env Attack ms", 5.0, 0.0, 2000.0, None),
    ("fenv_decay", "F.Env Decay ms", 300.0, 0.0, 5000.0, None),
    ("fenv_sustain", "F.Env Sustain", 0.2, 0.0, 1.0, None),
    ("fenv_release", "F.Env Release ms", 300.0, 0.0, 5000.0, None),
    ("menu_sync", "Auto Menu Sync", 0, 0, 1, TOG),
    ("sample_id", "Sample ID", 0, 0, 9999, INT),  # the recording "Sample" plays; set by Record
] + [c for sec, nm in (("vol", "Vol"), ("flt", "Filter"), ("pit", "Pitch"), ("pan", "Pan")) for c in (
    (f"lfo_{sec}_depth", f"{nm} LFO Depth", 0.0, 0.0, 100.0, None),
    (f"lfo_{sec}_rate", f"{nm} LFO Rate Hz", 5.0, 0.1, 12.0, None),
    (f"lfo_{sec}_delay", f"{nm} LFO Delay ms", 0.0, 0.0, 2000.0, None),
)]

def num(v):
    return f"{v:.1f}" if isinstance(v, float) else str(v)

def port(idx, c):
    sym, name, d, lo, hi, kind = c
    lines = [
        "    [",
        "        a lv2:InputPort, lv2:ControlPort ;",
        f"        lv2:index {idx} ;",
        f'        lv2:symbol "{sym}" ;',
        f'        lv2:name "{name}" ;',
        f"        lv2:default {num(d)} ;",
        f"        lv2:minimum {num(lo)} ;",
        f"        lv2:maximum {num(hi)} ;",
    ]
    if kind == INT:
        lines.append("        lv2:portProperty lv2:integer ;")
    elif kind == TOG:
        lines.append("        lv2:portProperty lv2:toggled ;")
    elif isinstance(kind, list):
        sp = " ,\n                       ".join(f'[ rdfs:label "{l}" ; rdf:value {i} ]' for i, l in enumerate(kind))
        lines.append("        lv2:portProperty lv2:integer, lv2:enumeration ;")
        lines.append(f"        lv2:scalePoint {sp} ;")
    lines.append("    ]")
    return "\n".join(lines)

def audio(idx, io, sym, name):
    return f"""    [
        a lv2:{io}Port, lv2:AudioPort ;
        lv2:index {idx} ;
        lv2:symbol "{sym}" ;
        lv2:name "{name}" ;
    ]"""

ports = ["""    [
        a lv2:InputPort, atom:AtomPort ;
        atom:bufferType atom:Sequence ;
        atom:supports midi:MidiEvent ;
        lv2:designation lv2:control ;
        lv2:index 0 ;
        lv2:symbol "events" ;
        lv2:name "Events" ;
    ]""",
         audio(1, "Output", "audio_out_l", "Audio Output Left"), audio(2, "Output", "audio_out_r", "Audio Output Right"),
         audio(3, "Input", "audio_in_l", "Audio Input Left"), audio(4, "Input", "audio_in_r", "Audio Input Right")]
ports += [port(5 + i, c) for i, c in enumerate(CONTROLS)]

ttl = f"""@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix foaf: <http://xmlns.com/foaf/0.1/> .
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix rdf:  <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix urid: <http://lv2plug.in/ns/ext/urid#> .

<{URI}>
    a lv2:InstrumentPlugin, lv2:Plugin ;
    doap:name "KeySampler" ;
    doap:maintainer [
        foaf:name "KeySampler" ;
    ] ;
    doap:license "MIT" ;
    lv2:minorVersion {MINOR} ;
    lv2:microVersion 0 ;
    rdfs:comment "Records the bus input into slots and plays it chromatically from a MIDI keyboard." ;
    lv2:requiredFeature urid:map ;
    lv2:extensionData state:interface ;
    lv2:port
""" + " ,\n".join(ports) + " .\n"

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "keysampler.ttl")
open(out, "w").write(ttl)
# defaults for the C code (Settings > Default), in port order
hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "port_defaults.h")
open(hdr, "w").write("/* generated by gen_ttl.py - port defaults in port order */\n"
                     "static const float port_default[] = { 0, 0, 0, 0, 0,\n    "
                     + ", ".join(num(c[2]) + ("f" if isinstance(c[2], float) else "") for c in CONTROLS) + " };\n")
print(f"wrote {out}: {len(CONTROLS)} controls, first 32 shown on the S2400 end with '{CONTROLS[31][1]}'")
