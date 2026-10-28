/* LFO test: LFO depth 100, 4 Hz; env B4 = number of B4 presses (0 pitch, 1 filter, 2 volume).
 * usage: host17 <plugin-uri> <out.wav>   env B4, FILTER, CUTOFF, RESO, SAMPLE */
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 48000
#define BLOCK 64
static char* uris[512]; static int nuris;
static LilvWorld* gw; static const LilvPlugin* gp;
static int idx(const char* sym) { LilvNode* n = lilv_new_string(gw, sym); const LilvPort* port = lilv_plugin_get_port_by_symbol(gp, n); lilv_node_free(n); if (!port) { fprintf(stderr, "no port %s\n", sym); exit(9); } return (int)lilv_port_get_index(gp, port); }
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* u) {
    (void)h;
    for (int i = 0; i < nuris; ++i) if (!strcmp(uris[i], u)) return i + 1;
    uris[nuris] = strdup(u); return ++nuris;
}
typedef struct { double t; int n; uint8_t m[8]; } Ev;
#define CC(t, c, v) { t, 3, { 0xB0, c, v } }
#define ON(t) { t, 3, { 0x90, 60, 127 } }
#define OFF(t) { t, 3, { 0x80, 60, 0 } }

int main(int argc, char** argv) {
    (void)argc;
    LilvWorld* w = lilv_world_new();
    lilv_world_load_all(w);
    LilvNode* uri = lilv_new_uri(w, argv[1]);
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), uri);
    gw = w; gp = p;
    LV2_URID_Map map = { NULL, map_uri };
    LV2_Feature mf = { LV2_URID__map, &map };
    const LV2_Feature* feats[] = { &mf, NULL };
    LilvInstance* in = lilv_plugin_instantiate(p, RATE, feats);
    uint32_t np = lilv_plugin_get_num_ports(p);
    float* ctl = calloc(np, 4), *mn = calloc(np, 4), *mx = calloc(np, 4);
    lilv_plugin_get_port_ranges_float(p, mn, mx, ctl);
    float L[BLOCK], R[BLOCK], IL[BLOCK] = {0}, IR[BLOCK] = {0};
    struct { LV2_Atom_Sequence seq; uint8_t buf[2048]; } mid;
    for (uint32_t i = 0; i < np; ++i) lilv_instance_connect_port(in, i, &ctl[i]);
    lilv_instance_connect_port(in, 0, &mid);
    lilv_instance_connect_port(in, 1, L); lilv_instance_connect_port(in, 2, R);
    lilv_instance_connect_port(in, 3, IL); lilv_instance_connect_port(in, 4, IR);
    ctl[idx("sample")] = getenv("SAMPLE") ? atof(getenv("SAMPLE")) : 5; ctl[idx("mode")] = getenv("MODE") ? atof(getenv("MODE")) : 2;
    ctl[idx("filter")] = getenv("FILTER") ? atof(getenv("FILTER")) : 1; ctl[idx("cutoff")] = getenv("CUTOFF") ? atof(getenv("CUTOFF")) : 50; ctl[idx("resonance")] = getenv("RESO") ? atof(getenv("RESO")) : 0;
    ctl[idx("attack")] = 0; ctl[idx("sustain")] = 1;
    /* LFO=vol|flt|pit|pan: that LFO at depth 100, 4 Hz, set in the menu */
    if (getenv("LFO")) { char n[40]; snprintf(n, sizeof n, "lfo_%s_depth", getenv("LFO")); ctl[idx(n)] = 100;
                         snprintf(n, sizeof n, "lfo_%s_rate", getenv("LFO")); ctl[idx(n)] = 4; }
    if (getenv("MODU")) ctl[idx("modulation")] = atof(getenv("MODU"));   /* 0 Volume, 1 Filter, 2 Pitch, 3 Pan */
    if (getenv("LSMS")) ctl[idx("loop_start_msec")] = atof(getenv("LSMS"));
    if (getenv("LEMS")) ctl[idx("loop_end_msec")] = atof(getenv("LEMS"));
    lilv_instance_activate(in);

    #define ON2(t,n) { t, 3, { 0x90, n, 127 } }
    #define OFF2(t,n) { t, 3, { 0x80, n, 0 } }
    #define CCX(t,c,v) { t, 3, { 0xB0, c, v } }
    /* PRESS="4", "55", "46" ...: B4/B5/B6 presses, then R4 (depth) to max and R3 (rate) to ~4 Hz */
    const char* pr = getenv("PRESS");
    Ev ev[16]; int ne = 0;
    for (int k = 0; pr && pr[k]; ++k) {
        double t = 0.02 + 0.25 * k;
        if (pr[k] == '4') { Ev b = CCX(t, 127, 0); ev[ne++] = b; }
        else if (pr[k] == '5') { Ev b = CCX(t, 126, 1); ev[ne++] = b; }
        else if (pr[k] == '6') { Ev b = { t, 6, { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 } }; ev[ne++] = b; }
        else if (pr[k] == 'p') { Ev b = CCX(t, 87, 127); ev[ne++] = b; }   /* B6 re-set to CC 87 */
    }
    if (pr) { Ev d = CCX(0.08 + 0.25 * strlen(pr), 77, 127), r = CCX(0.08 + 0.25 * strlen(pr), 76, 73); ev[ne++] = d; ev[ne++] = r; }
    { Ev on = ON2(pr ? 0.15 + 0.25 * strlen(pr) : 0.10, 48); Ev off = OFF2(pr ? 2.40 + 0.25 * strlen(pr) : 2.30, 48); ev[ne++] = on; ev[ne++] = off; }
    int nev = ne, ei = 0;
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);
    uint32_t total = (uint32_t)(RATE * (2.6 + (pr ? 0.25 * strlen(pr) + 0.1 : 0)));
    int16_t* out = calloc(total * 2, 2);
    for (uint32_t pos = 0; pos < total; pos += BLOCK) {
        /* UI=1: menu LFO Depth 100 and Rate 4 Hz, entered 50 ms after start (for the LFO Modulation selects) */
        if (getenv("UI") && pos >= RATE / 20) { ctl[idx("lfo_depth")] = 100; ctl[idx("lfo_rate")] = 4; }
        mid.seq.atom.type = seq_t; mid.seq.atom.size = sizeof(LV2_Atom_Sequence_Body);
        mid.seq.body.unit = 0; mid.seq.body.pad = 0;
        while (ei < nev && (uint32_t)(ev[ei].t * RATE) < pos + BLOCK) {
            struct { LV2_Atom_Event e; uint8_t m[16]; } x;
            int64_t f = (int64_t)(ev[ei].t * RATE) - pos;
            x.e.time.frames = f < 0 ? 0 : f;
            x.e.body.type = midi_t; x.e.body.size = ev[ei].n;
            memcpy(x.m, ev[ei].m, ev[ei].n);
            lv2_atom_sequence_append_event(&mid.seq, sizeof mid.buf + sizeof(LV2_Atom_Sequence_Body), &x.e);
            ++ei;
        }
        { }
        lilv_instance_run(in, BLOCK);
        for (int i = 0; i < BLOCK && pos + i < total; ++i) {
            out[(pos + i) * 2] = (int16_t)(fmaxf(-1, fminf(1, L[i])) * 32767);
            out[(pos + i) * 2 + 1] = (int16_t)(fmaxf(-1, fminf(1, R[i])) * 32767);
        }
        usleep(1333);
    }
    FILE* f = fopen(argv[2], "wb");
    uint32_t ds = total * 4, rs = 36 + ds, sr = RATE, br = RATE * 4, fs = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fs, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    fwrite(out, 4, total, f); fclose(f);
    lilv_instance_deactivate(in); lilv_instance_free(in); lilv_world_free(w);
    return 0;
}
