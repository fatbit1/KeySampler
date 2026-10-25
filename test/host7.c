/* Start / loop point sync test (sec / ms / smp parameters and CC point control).
 * usage: host7 <plugin-uri> <out.wav> */
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

    Ev ev[] = {
        CC(0.02, 126, 1),                                  /* B5: Gate -> Loop */
        ON(0.1), OFF(0.5),                                 /* A: 440 */
        CC(0.6, 101, 0), CC(0.6, 100, 2), CC(0.6, 6, 76),  /* R6 coarse +12 */
        ON(0.7), OFF(1.1),                                 /* B: 880 */
        CC(1.2, 101, 0), CC(1.2, 100, 1), CC(1.2, 6, 96), CC(1.2, 38, 0), /* R7 fine +50 cents */
        ON(1.3), OFF(1.7),                                 /* C: 906 */
        { 1.8, 2, { 0xC0, 1 } },                           /* B2: program +  -> slot 2 (empty) */
        ON(1.9), OFF(2.3),                                 /* D: silent */
        { 2.4, 2, { 0xC0, 0 } },                           /* B1: program -  -> slot 1 */
        ON(2.5), OFF(2.9),                                 /* E: 906 */
        CC(3.0, 10, 0),                                    /* R8 pan hard left */
        ON(3.1), OFF(3.5),                                 /* F: left only */
        CC(3.6, 10, 64), CC(3.6, 77, 127), CC(3.6, 76, 64), /* centre, vibrato depth max, rate mid */
        ON(3.7), OFF(4.7),                                 /* G: vibrato */
        CC(4.75, 77, 0), CC(4.8, 127, 0), CC(4.8, 74, 0),  /* vib off, B4: filter Off -> Low Pass, R1 cutoff min */
        ON(4.9), OFF(5.3),                                 /* H: filtered away */
        { 5.35, 6, { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 } }, /* B6: thru toggle (input silent here) */
        CC(5.38, 5, 64), CC(5.38, 91, 127), CC(5.38, 74, 127),
    };
    int nev = sizeof ev / sizeof ev[0], ei = 0;
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);
    uint32_t total = (uint32_t)(RATE * 6.2);
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
