/* State save/restore test, using the same lilv calls sushi uses for sessions.
 *   host4 <uri> save <out.state>            save state of a fresh instance
 *   host4 <uri> load <in.state> <out.wav>   restore into a fresh instance, play note 60 */
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>
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
static const char* unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID id) { (void)h; return id && id <= (LV2_URID)nuris ? uris[id - 1] : NULL; }

static float* g_ctl;
static const void* get_port(const char* sym, void* ud, uint32_t* size, uint32_t* type) {
    (void)sym; (void)ud; *size = 0; *type = 0; return NULL; /* params are saved separately by sushi */
}
static void set_port(const char* sym, void* ud, const void* v, uint32_t size, uint32_t type) {
    (void)sym; (void)ud; (void)v; (void)size; (void)type;
}

int main(int argc, char** argv) {
    LilvWorld* w = lilv_world_new();
    lilv_world_load_all(w);
    LilvNode* uri = lilv_new_uri(w, argv[1]);
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), uri);
    LV2_URID_Map map = { NULL, map_uri };
    LV2_URID_Unmap unmap = { NULL, unmap_uri };
    LV2_Feature mf = { LV2_URID__map, &map }, uf = { LV2_URID__unmap, &unmap };
    const LV2_Feature* feats[] = { &mf, &uf, NULL };
    LilvInstance* in = lilv_plugin_instantiate(p, RATE, feats);
    uint32_t np = lilv_plugin_get_num_ports(p);
    float* ctl = calloc(np, 4), *mn = calloc(np, 4), *mx = calloc(np, 4);
    g_ctl = ctl;
    lilv_plugin_get_port_ranges_float(p, mn, mx, ctl);
    float L[BLOCK], R[BLOCK], IL[BLOCK] = {0}, IR[BLOCK] = {0};
    struct { LV2_Atom_Sequence seq; uint8_t buf[1024]; } mid;
    for (uint32_t i = 0; i < np; ++i) lilv_instance_connect_port(in, i, &ctl[i]);
    lilv_instance_connect_port(in, 0, &mid);
    lilv_instance_connect_port(in, 1, L); lilv_instance_connect_port(in, 2, R);
    lilv_instance_connect_port(in, 3, IL); lilv_instance_connect_port(in, 4, IR);
    { LilvNode* n = lilv_new_string(w, "slot"); ctl[lilv_port_get_index(p, lilv_plugin_get_port_by_symbol(p, n))] = 2; lilv_node_free(n); }
    lilv_instance_activate(in);
    LV2_URID midi_t = map_uri(NULL, LV2_MIDI__MidiEvent), seq_t = map_uri(NULL, LV2_ATOM__Sequence);

    if (!strcmp(argv[2], "save")) {
        LilvState* st = lilv_state_new_from_instance(p, in, &map, NULL, NULL, NULL, NULL,
                                                     get_port, NULL, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, feats);
        char* str = lilv_state_to_string(w, &map, &unmap, st, "urn:test:state", NULL);
        FILE* f = fopen(argv[3], "w"); fputs(str, f); fclose(f);
        printf("saved state: %zu bytes\n", strlen(str));
        lilv_free(str); lilv_state_free(st);
    } else {
        FILE* f = fopen(argv[3], "r"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char* str = calloc(n + 1, 1); fread(str, 1, n, f); fclose(f);
        LilvState* st = lilv_state_new_from_string(w, &map, str);
        if (!st) { fprintf(stderr, "state parse failed\n"); return 4; }
        lilv_state_restore(st, in, set_port, NULL, 0, feats);
        lilv_state_free(st);
        uint32_t total = RATE, ei = 0;
        int16_t* out = calloc(total, 2);
        for (uint32_t pos = 0; pos < total; pos += BLOCK) {
            mid.seq.atom.type = seq_t; mid.seq.atom.size = sizeof(LV2_Atom_Sequence_Body);
            mid.seq.body.unit = 0; mid.seq.body.pad = 0;
            if (!ei && pos >= RATE / 2) { /* note 60 after 0.5 s */
                struct { LV2_Atom_Event e; uint8_t m[4]; } x = { { { 0 }, { 3, midi_t } }, { 0x90, 60, 127 } };
                lv2_atom_sequence_append_event(&mid.seq, sizeof mid.buf + sizeof(LV2_Atom_Sequence_Body), &x.e);
                ei = 1;
            }
            lilv_instance_run(in, BLOCK);
            for (int i = 0; i < BLOCK; ++i) out[pos + i] = (int16_t)(L[i] * 32767);
            usleep(1333);
        }
        f = fopen(argv[4], "wb");
        uint32_t ds = total * 2, rs = 36 + ds, sr = RATE, br = RATE * 2, fs = 16;
        uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fs, 4, 1, f);
        fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
        fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
        fwrite(out, 2, total, f); fclose(f);
    }
    lilv_instance_deactivate(in); lilv_instance_free(in); lilv_world_free(w);
    return 0;
}
