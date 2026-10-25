"""Generate a PCR-M30/M50/M80 template ("mask") for KeySampler's default CC map.

Geometry is traced from the GM2 template drawing in the PCR-M30/50/80 owner's
manual (p. 98) and scaled so R1-R8 knob centres are 175 mm apart (25 mm pitch).
Outputs SVG fragments used by the A3 and 2 x A4 HTML pages.
"""
S = 175.0 / 667.0          # mm per pixel of the 300-dpi manual drawing
X0, Y0 = 57, 28            # drawing origin (left edge of L-card, top of strip)

def X(px): return (px - X0) * S
def Y(px): return (px - Y0) * S

INK, SOFT, ACC, CUT = "#111", "#555", "#c2410c", "#8a8f98"

def text(x, y, s, size=2.6, weight=600, anchor="middle", fill=INK, ls=0.05):
    s = s.replace("&", "&amp;")
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-size="{size}" font-weight="{weight}" '
            f'text-anchor="{anchor}" fill="{fill}" letter-spacing="{ls}">{s}</text>')

def rrect(x1, y1, x2, y2, r, stroke=INK, w=0.35, dash=None, fill="none"):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<rect x="{X(x1):.2f}" y="{Y(y1):.2f}" width="{(x2-x1)*S:.2f}" height="{(y2-y1)*S:.2f}" '
            f'rx="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{w}"{d}/>')

def group_label(x1, x2, y, label):
    """Label centred over a span, with a rule each side (like the Roland template)."""
    xm = (X(x1) + X(x2)) / 2
    out = [text(xm, Y(y), label, 2.5, 800, fill=ACC, ls=0.12)]
    half = len(label) * 0.95 + 1.2
    out.append(f'<line x1="{X(x1):.2f}" y1="{Y(y)-0.85:.2f}" x2="{xm-half:.2f}" y2="{Y(y)-0.85:.2f}" stroke="{ACC}" stroke-width="0.25"/>')
    out.append(f'<line x1="{xm+half:.2f}" y1="{Y(y)-0.85:.2f}" x2="{X(x2):.2f}" y2="{Y(y)-0.85:.2f}" stroke="{ACC}" stroke-width="0.25"/>')
    return "".join(out)

def knob(cx, label, cc, ctrl):
    out = []
    r = 40 * S
    out.append(f'<circle cx="{X(cx):.2f}" cy="{Y(113):.2f}" r="{r:.2f}" fill="none" stroke="{CUT}" stroke-width="0.35" stroke-dasharray="1.2 0.8"/>')
    import math
    for k in range(11):  # scale dots, 7 o'clock to 5 o'clock
        a = math.radians(225 - k * 27)
        rr = r + 1.3
        out.append(f'<circle cx="{X(cx) + rr*math.cos(a):.2f}" cy="{Y(113) - rr*math.sin(a):.2f}" r="0.28" fill="{INK}"/>')
    out.append(text(X(cx), Y(56), label, 2.45, 700))
    out.append(text(X(cx), Y(65.5), f"{ctrl} · {cc}", 1.75, 500, fill=SOFT))
    return "".join(out)

def mask_svg():
    o = []
    # ---- L1-L3 card (not used by KeySampler)
    o.append(rrect(57, 140, 310, 262, 1.2))
    o.append(rrect(70, 175, 296, 228, 0.8, stroke=CUT, dash="1.2 0.8"))
    for x, lab in [(105, "L1"), (183, "L2"), (262, "L3")]:
        o.append(text(X(x), Y(166), lab, 2.2, 700, fill=SOFT))
    o.append(text(X(183), Y(246), "not used by KeySampler", 1.9, 500, fill=SOFT))
    o.append(text(X(296), Y(257), "KeySampler", 1.6, 700, anchor="end", fill=ACC))

    # ---- main strip
    o.append(rrect(380, 28, 1925, 265, 3.0, w=0.45))
    # knob group labels
    o.append(group_label(395, 568, 40, "FILTER"))
    o.append(group_label(580, 860, 40, "VIBRATO"))
    o.append(group_label(872, 1046, 40, "TUNE"))
    for xb in (573, 866, 1053):
        o.append(f'<line x1="{X(xb):.2f}" y1="{Y(34):.2f}" x2="{X(xb):.2f}" y2="{Y(62):.2f}" stroke="{INK}" stroke-width="0.4"/>')
    knobs = [(433, "CUTOFF", "CC 74", "R1"), (527, "RESONANCE", "CC 71", "R2"), (621, "RATE", "CC 76", "R3"),
             (714, "DEPTH", "CC 77", "R4"), (819, "DELAY", "CC 78", "R5"), (913, "COARSE", "RPN 0/2", "R6"),
             (1006, "FINE", "RPN 0/1", "R7"), (1100, "PAN", "CC 10", "R8")]
    for k in knobs:
        o.append(knob(*k))

    # lower-left window: PCR's own buttons, labels kept
    o.append(rrect(398, 181, 690, 237, 0.8, stroke=CUT, dash="1.2 0.8"))
    for x, lab in [(432, "MEMORY"), (507, "MIDI CH"), (652, "EDIT")]:
        o.append(text(X(x), Y(176), lab, 1.9, 600, fill=SOFT))
    o.append(text(X(580), Y(170.5), "PROGRAM", 1.7, 600, fill=SOFT))
    o.append(text(X(580), Y(177), "CHANGE", 1.7, 600, fill=SOFT))
    o.append(text(X(544), Y(250), "PCR keyboard buttons", 1.7, 500, fill=SOFT))

    # lower-middle window: B1-B6
    o.append(rrect(714, 181, 1136, 237, 0.8, stroke=CUT, dash="1.2 0.8"))
    btns = [(752, "RECORD", "Prog −", "B1"), (823, "FILTER", "Prog +", "B2"), (890, "MODE", "CC 65", "B3"),
            (960, "GLIDE", "CC 127", "B4"), (1030, "SAVE", "CC 126", "B5"), (1100, "RESTORE", "SysEx", "B6")]
    for x, lab, cc, b in btns:
        o.append(text(X(x), Y(177), lab, 2.3, 800, fill=ACC if lab == "RECORD" else INK))
        o.append(text(X(x), Y(250), f"{b} · {cc}", 1.7, 500, fill=SOFT))
    o.append(text(X(752), Y(256.5), "on / off", 1.45, 500, fill=SOFT))
    o.append(text(X(823), Y(256.5), "Off›LP›BP›HP", 1.45, 500, fill=SOFT))
    o.append(text(X(890), Y(256.5), "1-Shot›Gate›Loop", 1.45, 500, fill=SOFT))
    o.append(text(X(960), Y(256.5), "on / off", 1.45, 500, fill=SOFT))
    o.append(text(X(1030), Y(256.5), "changes › menu", 1.45, 500, fill=SOFT))
    o.append(text(X(1100), Y(256.5), "menu values", 1.45, 500, fill=SOFT))

    # display window
    o.append(rrect(1160, 107, 1335, 232, 0.8, stroke=CUT, dash="1.2 0.8"))
    for x, lab in [(1186, "USB"), (1240, "DATA OUT"), (1290, "HEX")]:
        o.append(text(X(x), Y(97), lab, 1.6, 600, fill=SOFT))
    o.append(text(X(1247), Y(66), "KeySampler", 4.2, 800, fill=INK, ls=-0.05))
    o.append(text(X(1247), Y(80), "S2400 DSP CARD", 1.7, 700, fill=ACC, ls=0.25))

    # slider windows
    o.append(rrect(1360, 67, 1616, 233, 0.8, stroke=CUT, dash="1.2 0.8"))
    o.append(rrect(1640, 67, 1895, 233, 0.8, stroke=CUT, dash="1.2 0.8"))
    o.append(group_label(1362, 1488, 37, "SAMPLE"))
    o.append(group_label(1500, 1762, 37, "ENVELOPE"))
    o.append(f'<line x1="{X(1770):.2f}" y1="{Y(32):.2f}" x2="{X(1770):.2f}" y2="{Y(64):.2f}" stroke="{INK}" stroke-width="0.4"/>')
    sliders = [(1394, "START", "CC 5", "S1"), (1458, "LOOP ST", "A.Touch", "S2"), (1524, "ATTACK", "CC 73", "S3"),
               (1588, "DECAY", "CC 75", "S4"), (1672, "RELEASE", "CC 72", "S5"), (1737, "SUSTAIN", "CC 93", "S6"),
               (1803, "LOOP END", "CC 91", "S7"), (1868, "LEVEL", "CC 7", "S8")]
    for x, lab, cc, sn in sliders:
        o.append(text(X(x), Y(55), lab, 2.05, 700))
        o.append(text(X(x), Y(62.5), cc, 1.6, 500, fill=SOFT))
    for xb in (1494,):
        o.append(f'<line x1="{X(xb):.2f}" y1="{Y(32):.2f}" x2="{X(xb):.2f}" y2="{Y(64):.2f}" stroke="{INK}" stroke-width="0.4"/>')
    o.append(text(X(1803), Y(37), "", 2))
    # footer
    o.append(text(X(1895), Y(250), "P1 HOLD (CC 64) · P2 EXPRESSION (CC 11) · LEVER PITCH BEND", 1.7, 600, anchor="end", fill=SOFT))
    o.append(text(X(1895), Y(257), "KEYSAMPLER MAP · PCR MEMORY No.0 : GM2", 1.6, 700, anchor="end", fill=INK, ls=0.15))
    return "".join(o)

W = X(1925) + 0.5   # total width in mm
H = Y(265) + 0.5
