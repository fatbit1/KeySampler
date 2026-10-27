/* lilv test host for KeySampler v3 (record from input, play via MIDI).
 * Uses the SAME buffers for audio in and out (in-place), runs at real-time pace.
 * usage: host3 <uri> <out.wav> <scenario: record|reload> */
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
enum { P_IN_L = 3, P_IN_R = 4 };
static LilvWorld* gw; static const LilvPlugin* gp;
static int idx(const char* sym) {
    LilvNode* n = lilv_new_string(gw, sym);
    const LilvPort* port = lilv_plugin_get_port_by_symbol(gp, n);
    lilv_node_free(n);
    if (!port) { fprintf(stderr, "no port %s\n", sym); exit(9); }
    return (int)lilv_port_get_index(gp, port);
}
static void set_ms(float* ctl, const char* pfx, double ms) {
    char a[64], b[64];
    snprintf(a, sizeof a, "%s_sec", pfx); snprintf(b, sizeof b, "%s_msec", pfx);
    ctl[idx(a)] = (float)floor(ms / 1000); ctl[idx(b)] = (float)fmod(ms, 1000);
}

static char* uris[512]; static int nuris;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* u) {
    (void)h;
    for (int i = 0; i < nuris; ++i) if (!strcmp(uris[i], u)) return i + 1;
    uris[nuris] = strdup(u); return ++nuris;
}
typedef struct { double t; uint8_t m[3]; } Ev;

int main(int argc, char** argv) {
    if (argc < 4) return 1;
    int reload = !strcmp(argv[3], "reload");
    int cctest = !strcmp(argv[3], "cc");
    int loop = !strcmp(argv[3], "loop");
    LilvWorld* w = lilv_world_new();
    lilv_world_load_all(w);
    LilvNode* uri = lilv_new_uri(w, argv[1]);
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), uri);
    if (!p) { fprintf(stderr, "plugin not found\n"); return 2; }
    gw = w; gp = p;
    LV2_URID_Map map = { NULL, map_uri };
    LV2_Feature mf = { LV2_URID__map, &map };
    const LV2_Feature* feats[] = { &mf, NULL };
    LilvInstance* in = lilv_plugin_instantiate(p, RATE, feats);
    if (!in) { fprintf(stderr, "instantiate failed\n"); return 3; }

    uint32_t np = lilv_plugin_get_num_ports(p);
    float* ctl = calloc(np, 4), *mn = calloc(np, 4), *mx = calloc(np, 4);
    lilv_plugin_get_port_ranges_float(p, mn, mx, ctl);
    float L[BLOCK], R[BLOCK];
    float smp_no = getenv("SAMPLE") ? atof(getenv("SAMPLE")) : 0;
    float seq_v = -1;
    double hz = getenv("HZ") ? atof(getenv("HZ")) : 330;
    struct { LV2_Atom_Sequence seq; uint8_t buf[4096]; } mid;
    for (uint32_t i = 0; i < np; ++i) lilv_instance_connect_port(in, i, &ctl[i]);
    lilv_instance_connect_port(in, 0, &mid);
    lilv_instance_connect_port(in, 1, L);  lilv_instance_connect_port(in, 2, R);
    lilv_instance_connect_port(in, P_IN_L, L); lilv_instance_connect_port(in, P_IN_R, R); /* in-place */
    lilv_instance_activate(in);

    Ev rec_ev[] = { {2.2, {0x90, 60, 127}}, {2.8, {0x80, 60, 0}},
                    {3.0, {0x90, 72, 127}}, {3.5, {0x80, 72, 0}},
                    {3.9, {0x90, 69, 127}}, {4.4, {0x80, 69, 0}},
                    {4.8, {0x90, 67, 127}}, {5.3, {0x80, 67, 0}} };
    Ev rl_ev[]  = { {0.5, {0x90, 60, 127}}, {1.1, {0x80, 60, 0}} };
    Ev lp_ev[]  = { {0.1, {0x90, 60, 127}}, {3.0, {0x80, 60, 0}} };
    Ev cc_ev[] = { {0.1, {0x90, 60, 127}}, {0.5, {0xB0, 74, 0}}, {1.0, {0xB0, 74, 127}},
                   {2.2, {0xB0, 20, 0}}, {2.7, {0xB0, 20, 127}}, {3.4, {0x80, 60, 0}} };
    Ev* ev = cctest ? cc_ev : loop ? lp_ev : reload ? rl_ev : rec_ev;
    int nev = cctest ? 6 : loop ? 2 : reload ? 2 : 8, ei = 0;
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);

    double dur = cctest ? 3.3 : loop ? 3.0 : reload ? 1.5 : 5.5;
    uint32_t total = (uint32_t)(RATE * dur);
    int16_t* out = calloc(total * 2, 2);
    for (uint32_t pos = 0; pos < total; pos += BLOCK) {
        double now = (double)pos / RATE;
        if (cctest) {
            ctl[idx("sample")] = 0; ctl[idx("mode")] = 2; ctl[idx("filter")] = 1;
            ctl[idx("cutoff")] = now >= 1.5 ? 30 : 100;
        } else if (loop) { ctl[idx("sample")] = 0; ctl[idx("mode")] = 2; set_ms(ctl, "loop_start", atof(argv[4])); set_ms(ctl, "loop_end", atof(argv[5]));
            if (argc > 8) { ctl[idx("filter")] = atof(argv[6]); ctl[idx("cutoff")] = atof(argv[7]); ctl[idx("resonance")] = atof(argv[8]); }
            if (getenv("START")) set_ms(ctl, "start", atof(getenv("START")));
            if (getenv("LSS")) ctl[idx("loop_start_smp")] = atof(getenv("LSS")); }
        else if (reload) ctl[idx("sample")] = smp_no;
        if (getenv("SAMPLE_ID")) ctl[idx("sample_id")] = atof(getenv("SAMPLE_ID"));
        else {
            ctl[idx("sample")] = smp_no;
            /* ROUNDTRIP=1: like the S2400 after a recording, send back the newest number the
             * way it travels (normalized 0..1 as float, then scaled again) */
            if (getenv("ROUNDTRIP") && now >= 2.1) {
                static float back = -1;
                if (back < 0) {
                    char d[1200]; snprintf(d, sizeof d, "%s", getenv("RECDIR") ? getenv("RECDIR") : ".");
                    int hi = 0; for (int n = 1; n <= 9999; ++n) { char f[1300]; snprintf(f, sizeof f, "%s/sample_%04d.wav", d, n); FILE* t = fopen(f, "rb"); if (t) { fclose(t); hi = n; } }
                    float norm = (float)hi / 9999.f; back = norm * 9999.f;
                    fprintf(stderr, "roundtrip: newest recording %d comes back as Sample ID %.7f\n", hi, back);
                }
                ctl[idx("sample")] = (4.f / 10003.f) * 10003.f;   /* Sound = "Sample" */
                ctl[idx("sample_id")] = back;
            }
            /* NOECHO=1: the menu sends Sound = Sample, but Sample ID stays 0 */
            if (getenv("NOECHO") && now >= 2.1) { ctl[idx("sample")] = 4; ctl[idx("sample_id")] = 0; }
            /* SEQ="t:v,...": change Sample at those times (after the round trip, if any) */
            if (getenv("SEQ")) { const char* c = getenv("SEQ"); float v = -1;
                while (c && *c) { double t0; float v0; if (sscanf(c, "%lf:%f", &t0, &v0) != 2) break;
                    if (now >= t0) v = v0; c = strchr(c, ','); if (c) ++c; }
                if (v >= 0) seq_v = v; if (seq_v >= 0) ctl[idx("sample")] = seq_v; }
            ctl[idx("record")] = (now >= 0.2 && now < 2.0) ? 1 : 0;
        }
        /* input: the "pad hit" - 330 Hz with harmonics, sharp attack, 0.6 s decay */
        for (int i = 0; i < BLOCK; ++i) {
            double t = (double)(pos + i) / RATE - 0.5;
            float x = 0;
            if (!reload && !loop && !cctest && t >= 0 && t < 0.6)
                x = (float)(0.5 * exp(-t * 4) * (sin(2 * M_PI * hz * t) + 0.3 * sin(2 * M_PI * 2 * hz * t)));
            L[i] = R[i] = x;
        }
        mid.seq.atom.type = seq_t; mid.seq.atom.size = sizeof(LV2_Atom_Sequence_Body);
        mid.seq.body.unit = 0; mid.seq.body.pad = 0;
        while (ei < nev && (uint32_t)(ev[ei].t * RATE) < pos + BLOCK) {
            struct { LV2_Atom_Event e; uint8_t m[8]; } x;
            x.e.time.frames = (int64_t)(ev[ei].t * RATE) - pos;
            if (x.e.time.frames < 0) x.e.time.frames = 0;
            x.e.body.type = midi_t; x.e.body.size = 3;
            memcpy(x.m, ev[ei].m, 3);
            lv2_atom_sequence_append_event(&mid.seq, sizeof mid.buf + sizeof(LV2_Atom_Sequence_Body), &x.e);
            ++ei;
        }
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
    printf("rendered %.1f s\n", dur);
    return 0;
}
