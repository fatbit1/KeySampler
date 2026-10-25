/* Minimal lilv test host: instantiates a plugin the way sushi does
 * (lilv_plugin_instantiate), plays a MIDI test sequence, writes a stereo WAV.
 * usage: host <plugin-uri> <out.wav> [mode] */
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

static char* uris[64]; static int nuris;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* u) {
    (void)h;
    for (int i = 0; i < nuris; ++i) if (!strcmp(uris[i], u)) return i + 1;
    uris[nuris] = strdup(u); return ++nuris;
}

typedef struct { double t; uint8_t m[3]; } Ev;

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    float mode = argc > 3 ? atof(argv[3]) : 1;
    LilvWorld* w = lilv_world_new();
    lilv_world_load_all(w);
    LilvNode* uri = lilv_new_uri(w, argv[1]);
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), uri);
    if (!p) { fprintf(stderr, "plugin not found\n"); return 2; }

    LV2_URID_Map map = { NULL, map_uri };
    LV2_Feature mf = { LV2_URID__map, &map };
    const LV2_Feature* feats[] = { &mf, NULL };
    LilvInstance* in = lilv_plugin_instantiate(p, RATE, feats);
    if (!in) { fprintf(stderr, "instantiate failed\n"); return 3; }

    uint32_t np = lilv_plugin_get_num_ports(p);
    float* ctl = calloc(np, sizeof(float)), *mins = calloc(np, 4), *maxs = calloc(np, 4);
    lilv_plugin_get_port_ranges_float(p, mins, maxs, ctl);
    ctl[11] = mode;
    ctl[9] = 300; /* release */
    float L[BLOCK], R[BLOCK];
    struct { LV2_Atom_Sequence seq; uint8_t buf[4096]; } mid;
    for (uint32_t i = 0; i < np; ++i) lilv_instance_connect_port(in, i, &ctl[i]);
    lilv_instance_connect_port(in, 0, &mid);
    lilv_instance_connect_port(in, 1, L);
    lilv_instance_connect_port(in, 2, R);
    lilv_instance_activate(in);

    /* test: root note, octave up, fifth, chord, pitch bend, low note */
    Ev ev[] = {
        {0.00, {0x90, 69, 127}}, {0.40, {0x80, 69, 0}},
        {0.50, {0x90, 81, 100}}, {0.90, {0x80, 81, 0}},
        {1.00, {0x90, 76, 60}},  {1.40, {0x80, 76, 0}},
        {1.50, {0x90, 69, 110}}, {1.50, {0x90, 73, 110}}, {1.50, {0x90, 76, 110}},
        {2.00, {0xE0, 0, 127}},  {2.60, {0x80, 69, 0}}, {2.60, {0x80, 73, 0}}, {2.60, {0x80, 76, 0}},
        {2.61, {0xE0, 0, 64}},
        {2.80, {0x90, 57, 127}}, {3.60, {0x80, 57, 0}},
    };
    int nev = sizeof ev / sizeof ev[0], ei = 0;
    Ev hold[] = { {0.0, {0x90, 81, 127}}, {3.5, {0x80, 81, 0}} }; /* 2x speed: sample lasts 0.75 s */
    Ev* evp = ev;
    if (argc > 4 && !strcmp(argv[4], "hold")) { evp = hold; nev = 2; }
    Ev v2[] = { {0.2, {0x90, 69, 127}}, {0.8, {0x80, 69, 0}},
                {3.6, {0x90, 69, 127}}, {4.2, {0x80, 69, 0}},
                {4.9, {0x90, 69, 127}}, {5.5, {0x80, 69, 0}} };
    int v2mode = argc > 4 && !strcmp(argv[4], "v2");
    if (v2mode) { evp = v2; nev = 6; }
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);

    uint32_t total = RATE * (v2mode ? 6 : 4);
    int16_t* out = malloc(total * 4);
    float peak = 0;
    for (uint32_t pos = 0; pos < total; pos += BLOCK) {
        mid.seq.atom.type = seq_t; mid.seq.atom.size = sizeof(LV2_Atom_Sequence_Body);
        mid.seq.body.unit = 0; mid.seq.body.pad = 0;
        while (ei < nev && (uint32_t)(evp[ei].t * RATE) < pos + BLOCK) {
            struct { LV2_Atom_Event e; uint8_t m[8]; } x;
            x.e.time.frames = (int64_t)(evp[ei].t * RATE) - pos;
            x.e.body.type = midi_t; x.e.body.size = 3;
            memcpy(x.m, evp[ei].m, 3);
            lv2_atom_sequence_append_event(&mid.seq, sizeof mid.buf + sizeof(LV2_Atom_Sequence_Body), &x.e);
            ++ei;
        }
        if (v2mode) {
            double now = (double)pos / RATE;
            ctl[15] = now >= 4.5 ? 1 : now >= 1.0 ? 2 : 1;
            static int copied = 0;
            if (!copied && now >= 1.2) { copied = 1; system(argv[5]); fprintf(stderr, "[host] t=%.2fs new file added\n", now); }
            usleep(1333); /* run at real-time speed so the loader thread sees realistic timing */
        }
        lilv_instance_run(in, BLOCK);
        for (int i = 0; i < BLOCK; ++i) {
            float a = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
            if (a > peak) peak = a;
            out[(pos + i) * 2] = (int16_t)(fmaxf(-1, fminf(1, L[i])) * 32767);
            out[(pos + i) * 2 + 1] = (int16_t)(fmaxf(-1, fminf(1, R[i])) * 32767);
        }
    }
    FILE* f = fopen(argv[2], "wb");
    uint32_t ds = total * 4, rs = 36 + ds, sr = RATE, br = RATE * 4, fs = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fs, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    fwrite(out, 4, total, f); fclose(f);
    printf("rendered %u frames, peak %.3f\n", total, peak);
    lilv_instance_deactivate(in); lilv_instance_free(in);
    lilv_world_free(w);
    return 0;
}
