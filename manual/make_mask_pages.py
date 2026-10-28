import importlib.util
spec=importlib.util.spec_from_file_location("pm","pcr_mask.py"); pm=importlib.util.module_from_spec(spec); spec.loader.exec_module(pm)
full=pm.mask_svg()
i=full.index(pm.rrect(380, 28, 1925, 265, 3.0, w=0.45))
card, strip = full[:i], full[i:]
X=pm.X; Y=pm.Y
sx=X(380); SW=X(1925)-X(380); SH=Y(265)
cx0, cy0, CW, CH = X(57), Y(140), X(310)-X(57), Y(262)-Y(140)
strip_g=f'<g transform="translate({-sx:.2f},0)">{strip}</g>'
def card_g(x,y): return f'<g transform="translate({x-cx0:.2f},{y-cy0:.2f})">{card}</g>'
ruler=''.join(f'<line x1="{i}" y1="0" x2="{i}" y2="{3 if i%10==0 else 1.6}" stroke="#111" stroke-width="0.2"/>' for i in range(0,101))
ruler_svg=f'<svg width="106mm" height="8mm" viewBox="-2 -1 106 8">{ruler}<text x="0" y="6" font-size="2.2" font-family="Inter">0</text><text x="100" y="6" font-size="2.2" text-anchor="middle" font-family="Inter">100 mm</text></svg>'
cal=f'''<div class="cal">{ruler_svg}
<p><b>1. Check the print scale.</b> The bar above must measure exactly 100 mm (print at "actual size" / 100 %). On this mask the R1&ndash;R8 knob centres are 175 mm apart. Measure R1 to R8 on your PCR (centre to centre); if it differs, print again at <b>your measurement &divide; 175 &times; 100 %</b>.</p>
<p><b>2. Cut</b> along the outer edges and the grey dashed lines (knob holes, windows for the buttons, display and sliders). The small card goes over L1&ndash;L3 (not used by KeySampler).</p>
<p><b>Key:</b> black = KeySampler function; grey = control and the message the PCR sends (GM2 set, with L1-L3 set to CC 80-82 and saved as memory 1); orange = groups. The window marked "PCR keyboard buttons" keeps its normal PCR functions.</p></div>'''
css='''@page { margin: 0; } body { margin:0; font-family: Inter, sans-serif; background:#fff; } svg text { font-family: Inter, sans-serif; }
.pg { break-after: page; padding: 7mm 6mm 0; } .pg:last-child { break-after: auto; }
.cal { margin: 3mm 0 0; font-size: 8.3pt; line-height: 1.4; max-width: 255mm; color:#222 } .cal p { margin: 1.2mm 0 }
.lbl { font-size: 8pt; color:#555; margin: 1.5mm 0 } h1 { font-size: 11pt; margin: 0 0 2mm; }'''
def svgblock(w,h,inner,vbx=0): return f'<svg width="{w:.2f}mm" height="{h:.2f}mm" viewBox="{vbx:.2f} -0.5 {w:.2f} {h:.2f}" xmlns="http://www.w3.org/2000/svg">{inner}</svg>'
a3=f'''<!doctype html><html><head><meta charset="utf-8"><title>KeySampler PCR-M30 Mask A3</title><style>{css} @page {{ size: 420mm 297mm; }}</style></head><body><section class="pg">
<h1>KeySampler &middot; Edirol PCR-M30/M50/M80 template (memory 1: GM2 + L1-L3)</h1>
{svgblock(SW+1, SH+1, strip_g, -0.5)}<div style="height:5mm"></div>{svgblock(CW+1, CH+1, card_g(0,0), -0.5)}{cal}</section></body></html>'''
open('mask_a3.html','w').write(a3)
half=SW/2; ov=8
def cross(x,y): return f'<g stroke="#c2410c" stroke-width="0.25"><line x1="{x-3}" y1="{y}" x2="{x+3}" y2="{y}"/><line x1="{x}" y1="{y-3}" x2="{x}" y2="{y+3}"/><circle cx="{x}" cy="{y}" r="1.5" fill="none"/></g>'
marks=cross(half,2.5)+cross(half,SH-2.5)
trim=f'<line x1="{half+ov:.2f}" y1="-0.5" x2="{half+ov:.2f}" y2="{SH+0.5:.2f}" stroke="#c2410c" stroke-width="0.25" stroke-dasharray="2 1"/>'
w1=half+ov+0.5; w2=SW-(half-ov)+1
s1=svgblock(w1, SH+1, strip_g+marks+trim, -0.5); s2=svgblock(w2, SH+1, strip_g+marks, half-ov)
a4=f'''<!doctype html><html><head><meta charset="utf-8"><title>KeySampler PCR-M30 Mask A4</title><style>{css} @page {{ size: 297mm 210mm; }}</style></head><body>
<section class="pg"><h1>KeySampler &middot; PCR-M30 template &middot; sheet 1 of 2 (left half)</h1>{s1}
<p class="lbl">Trim this sheet along the orange dashed line. Lay it over sheet 2 so the orange crosses line up, and tape it on the back.</p>
<div style="height:3mm"></div>{svgblock(CW+1, CH+1, card_g(0,0), -0.5)}{cal}</section>
<section class="pg"><h1>KeySampler &middot; PCR-M30 template &middot; sheet 2 of 2 (right half)</h1>{s2}
<p class="lbl">Keep this sheet whole; sheet 1 overlaps it up to the orange crosses.</p></section></body></html>'''
open('mask_a4.html','w').write(a4)
print(f"strip {SW:.1f} x {SH:.1f} mm; A4 pieces {w1:.1f}/{w2:.1f} mm")
