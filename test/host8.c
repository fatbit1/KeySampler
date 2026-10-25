/* Glide test: portamento and legato.
 * usage: host8 <plugin-uri> <out.wav> */
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
    lilv_instance_activate(in);

    #define ON2(t,n) { t, 3, { 0x90, n, 127 } }
    #define OFF2(t,n) { t, 3, { 0x80, n, 0 } }
    Ev ev[] = {
        ON2(0.10, 60), OFF2(0.60, 60), ON2(0.70, 72), OFF2(1.40, 72),          /* portamento 500 ms */
        ON2(1.60, 60), ON2(2.00, 67), OFF2(2.60, 67), OFF2(3.00, 60),        /* legato 200 ms */
        ON2(3.40, 64), OFF2(3.90, 64),                                         /* legato, fresh note */
    };
    int nev = sizeof ev / sizeof ev[0], ei = 0;
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);
    uint32_t total = (uint32_t)(RATE * 4.2);
    int16_t* out = calloc(total * 2, 2);
    for (uint32_t pos = 0; pos < total; pos += BLOCK) {
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
        { double now = (double)pos / RATE;
          ctl[idx("mode")] = 2; ctl[idx("attack")] = 300;
          ctl[idx("glide_mode")] = now < 1.5 ? 1 : 2; ctl[idx("glide")] = now < 1.5 ? 500 : 200; }
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
