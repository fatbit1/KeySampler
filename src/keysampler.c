/*
 * KeySampler v3 - a recording, chromatic sample-playback LV2 instrument for the
 * ISLA S2400 DSP Card.
 *
 * Put it on a DSP bus. Route an S2400 track to that bus, switch Record on and
 * hit the pad: KeySampler records what comes into the bus (starting when the
 * sound starts, stopping when it goes quiet) into the current Slot. Then play
 * the recording chromatically from a MIDI keyboard or MIDI track.
 *
 *  - polyphonic (16 voices), ADSR, velocity, pitch bend, sustain pedal,
 *    One Shot / Gate / Loop modes
 *  - 64 slots; recordings are saved on the DSP Card's own storage
 *    (/home/isla/user/keysampler_recordings/slotNN.wav) and survive power-off
 *  - a slot with no recording falls back to the n-th WAV in the bundle's
 *    samples/ folder (so sounds can also be added via SD card + Sync Files)
 *  - Thru passes the bus audio on, so the bus keeps working as normal
 *
 * Real-time safety: the audio thread never allocates or touches the disk.
 * A background thread loads files, turns finished recordings into playable
 * samples and writes them to disk.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>

#include "grpc_lite.c"

#define KEYSAMPLER_URI "https://github.com/keysampler/KeySampler"
#define MAX_VOICES 16
#define MAX_FILES 128
#define MAX_SLOTS 64
#define MAX_BANKS 99
#define MAX_SEC 59         /* start/loop points: seconds + ms + samples, */
#define MAX_SMP 95         /* each a small whole number, like the S2400's s.mmm+samples */
/* a "key" identifies bank+slot: bank * 100 + slot */
#define KEY(bank, slot) ((bank) * 100 + (slot))
#define PATH_LEN 1024
#define REC_SECONDS 30
#define PREROLL 32         /* frames (~0.7 ms) kept before the trigger point */
#define SCRATCH 8192       /* max block size handled in one go */

/* Port order = order of the parameters in the S2400's plugin menu.
 * The S2400 shows only the first 32 controls (ports 5-36), so the settings
 * changed most often come first; the rest keep their defaults and can still be
 * reached by CC Learn / the PCR controls. */
enum {
    P_MIDI_IN = 0,
    P_OUT_L,
    P_OUT_R,
    P_IN_L,
    P_IN_R,
    /* ---- shown in the S2400 menu (32) ---- */
    P_SLOT,
    P_BANK,
    P_RECORD,
    P_THRESH,
    P_MODE,
    P_LEVEL,
    P_ROOT,
    P_TUNE,
    P_ATTACK,
    P_DECAY,
    P_SUSTAIN,
    P_RELEASE,
    P_START_S,
    P_START_MS,
    P_LOOP_START_S,
    P_LOOP_START_MS,
    P_LOOP_END_S,
    P_LOOP_END_MS,
    P_FILTER,
    P_CUTOFF,
    P_RESO,
    P_FENV_AMT,
    P_FENV_A,
    P_FENV_D,
    P_FENV_S,
    P_FENV_R,
    P_GLIDE_MODE,
    P_GLIDE,
    P_VIB_DEPTH,
    P_PAN,
    P_VOICES,
    P_LEARN,
    /* ---- beyond the S2400's 32: defaults, CC-controllable ---- */
    P_THRU,
    P_VIB_RATE,
    P_VELSENS,
    P_BEND,
    P_VIB_DELAY,
    P_START_SMP,
    P_LOOP_START_SMP,
    P_LOOP_END_SMP,
    P_FENV_ON,
    P_MENU_SYNC,
    P_COUNT
};
#define P_FIRST_CTL P_SLOT

/* ms and samples ports belonging to a point's "sec" port */
static int point_ms(int p_s) { return p_s == P_START_S ? P_START_MS : p_s == P_LOOP_START_S ? P_LOOP_START_MS : P_LOOP_END_MS; }
static int point_smp(int p_s) { return p_s == P_START_S ? P_START_SMP : p_s == P_LOOP_START_S ? P_LOOP_START_SMP : P_LOOP_END_SMP; }

/* Things a MIDI CC can control (also the "CC Learn" menu, in this order). */
enum { T_NONE = 0, T_LEVEL, T_CUTOFF, T_RESO, T_ATTACK, T_DECAY, T_SUSTAIN, T_RELEASE, T_TUNE,
       T_START, T_LOOP_START, T_LOOP_END, T_SLOT, T_RECORD, T_FILTER, T_MODE,
       T_PAN, T_VIB_RATE, T_VIB_DEPTH, T_VIB_DELAY, T_THRU, T_GLIDE_MODE, T_GLIDE,
       T_FENV_ON, T_FENV_AMT, T_FENV_A, T_FENV_D, T_FENV_S, T_FENV_R,
       T_SAVE, T_RESTORE, /* actions, not parameters */
       T_COUNT,
       T_RESET = T_COUNT };
static const int t_port[T_COUNT] = { -1, P_LEVEL, P_CUTOFF, P_RESO, P_ATTACK, P_DECAY, P_SUSTAIN,
    P_RELEASE, P_TUNE, P_START_S, P_LOOP_START_S, P_LOOP_END_S, P_SLOT, P_RECORD, P_FILTER, P_MODE,
    P_PAN, P_VIB_RATE, P_VIB_DEPTH, P_VIB_DELAY, P_THRU, P_GLIDE_MODE, P_GLIDE,
    P_FENV_ON, P_FENV_AMT, P_FENV_A, P_FENV_D, P_FENV_S, P_FENV_R, -1, -1 };

#define CC_PRESSURE 128    /* channel aftertouch, handled like a 129th CC */
#define CC_SLOTS 129
/* Defaults = the Edirol PCR-M30/50/80 "GM2 SET" (memory 0), owner's manual p. 98.
 * Knobs follow the printed template; the sliders carry ADSR and the points.
 * Fixed (not in this table): R6/R7 RPN coarse/fine tune, P1 hold, P2 expression,
 * Buttons: B1 (program -) = record on/off, B2 (program +) = filter type, B3 (CC65) = play
 * mode, B4 (CC127) = glide on/off, B5 (CC126) = save controller changes to the menu,
 * B6 (GM2 On SysEx) = restore the menu values. */
static const struct { int cc, t; } cc_defaults[] = {
    { 74, T_CUTOFF },     /* R1 FILTER CUTOFF */
    { 71, T_RESO },       /* R2 FILTER RESONANCE */
    { 76, T_VIB_RATE },   /* R3 VIBRATO RATE */
    { 77, T_VIB_DEPTH },  /* R4 VIBRATO DEPTH */
    { 78, T_VIB_DELAY },  /* R5 VIBRATO DELAY */
    { 10, T_PAN },        /* R8 PAN */
    { 5, T_START },       /* S1 (portamento time) -> sample start */
    { CC_PRESSURE, T_LOOP_START }, /* S2 (aftertouch) -> loop start */
    { 73, T_ATTACK },     /* S3 ENVELOPE ATTACK */
    { 75, T_DECAY },      /* S4 ENVELOPE DECAY */
    { 72, T_RELEASE },    /* S5 ENVELOPE RELEASE */
    { 93, T_SUSTAIN },    /* S6 (chorus) -> sustain level */
    { 91, T_LOOP_END },   /* S7 (reverb) -> loop end */
    { 7, T_LEVEL },       /* S8 VOLUME */
};

/* ---------------------------------------------------------- oscillators
 * Slots 61-64 play built-in waveforms. Each has one table per octave with only
 * the harmonics that stay below ~20 kHz in that octave (band-limited), so high
 * notes don't alias. Root Note plays middle C (261.63 Hz). */
#define OSC_FIRST 61
#define WT_SIZE 2048
#define WT_LEVELS 11
enum { OSC_SINE = 0, OSC_TRI, OSC_SQUARE, OSC_SAW };
static float* g_wt;  /* [4][WT_LEVELS][WT_SIZE + 1] */
static pthread_once_t g_wt_once = PTHREAD_ONCE_INIT;

static void build_wavetables(void)
{
    g_wt = calloc((size_t)4 * WT_LEVELS * (WT_SIZE + 1), sizeof(float));
    if (!g_wt) return;
    for (int w = 0; w < 4; ++w) {
        float norm = 1.f;
        for (int k = 0; k < WT_LEVELS; ++k) {
            float* t = g_wt + ((size_t)w * WT_LEVELS + k) * (WT_SIZE + 1);
            double fmax = 27.5 * pow(2.0, k + 1);         /* highest fundamental in this octave */
            int H = (int)(20000.0 / fmax);
            if (H < 1) H = 1;
            if (H > WT_SIZE / 2 - 1) H = WT_SIZE / 2 - 1;
            for (int n = 1; n <= (w == OSC_SINE ? 1 : H); ++n) {
                double a;
                if (w == OSC_SINE) a = 1.0;
                else if (w == OSC_TRI) a = (n & 1) ? (8.0 / (M_PI * M_PI)) * (((n - 1) / 2) & 1 ? -1.0 : 1.0) / ((double)n * n) : 0.0;
                else if (w == OSC_SQUARE) a = (n & 1) ? 4.0 / (M_PI * n) : 0.0;
                else a = (2.0 / M_PI) * ((n & 1) ? 1.0 : -1.0) / n;
                if (a == 0.0) continue;
                for (int i = 0; i < WT_SIZE; ++i) t[i] += (float)(a * sin(2.0 * M_PI * n * i / WT_SIZE));
            }
            if (k == 0) { /* same scale for every octave of a waveform: peak 0.8 */
                float pk = 0.f;
                for (int i = 0; i < WT_SIZE; ++i) pk = fmaxf(pk, fabsf(t[i]));
                norm = pk > 0 ? 0.8f / pk : 1.f;
            }
            for (int i = 0; i < WT_SIZE; ++i) t[i] *= norm;
            t[WT_SIZE] = t[0];
        }
    }
}

static inline float osc_sample(int w, int level, double phase)
{
    const float* t = g_wt + ((size_t)w * WT_LEVELS + level) * (WT_SIZE + 1);
    double x = phase * WT_SIZE;
    int i = (int)x;
    float f = (float)(x - i);
    return t[i] + f * (t[i + 1] - t[i]);
}

enum { MODE_ONESHOT = 0, MODE_GATE = 1, MODE_LOOP = 2 };
enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { REC_IDLE = 0, REC_ARMED, REC_RECORDING, REC_HANDOFF };

typedef struct {
    float*   l; /* de-interleaved; frame k stored at [k+1]; padded for interpolation */
    float*   r;
    uint32_t frames;
    double   rate;
    uint32_t loop_start, loop_end;
} Sample;

typedef struct {
    int    stage;
    int    note;
    int    held;
    int    sustained;
    double pos;
    float  env;
    float  vel_gain;
    float  rel_step;
    uint32_t age;
    uint32_t vib_t; /* frames since note-on, for vibrato delay */
    double   pitch, target, glide_step; /* current / target note (semitones), glide speed per frame */
    double   phase;                     /* oscillator slots */
    /* per-voice filter + its envelope */
    float    fic1[2], fic2[2];
    float    fa1, fa2, fa3;
    int      fstage;
    float    fenv, frel_step;
    uint32_t fcount;
} Voice;

typedef struct {
    const float* ctl[P_COUNT];
    const LV2_Atom_Sequence* midi_in;
    float* out_l;
    float* out_r;
    const float* in_l;
    const float* in_r;
    LV2_URID midi_event;
    /* MIDI CC: cc_map[cc] = target. A CC move overrides the menu value until
     * the menu value is changed again ("last touched wins"). */
    int8_t   cc_map[CC_SLOTS];
    int      rpn;                 /* selected RPN (CC101/100) */
    float    rpn_coarse, rpn_fine; /* semitones, cents */
    int      rpn_fine_msb;
    int      tune_is_rpn;
    int      last_program;
    int      glide_on_mode;       /* mode B4 switches back to (Portamento or Legato) */
    float    expr;                /* CC11 expression, 0..1 */
    int      held_keys[32], n_held;  /* keys down, oldest first (legato note priority) */
    double   last_pitch;          /* pitch of the latest note, where portamento starts */
    int      have_last;
    int8_t   port_target[P_COUNT];
    float    ov[T_COUNT];       /* 0..1 */
    int      ov_on[T_COUNT];
    double   ov_snap[T_COUNT];  /* menu value when the CC took over */
    int      learn_prev, learn_armed;
    atomic_int map_dirty;

    /* Menu sync: after a controller move, the loader thread asks sushi (the
     * DSP Card's audio engine) to set the same parameter, like the S2400 menu
     * does, so the new value can show up in the menu and be saved. */
    float    sync_val[P_COUNT];        /* domain values to send (audio thread writes) */
    _Atomic unsigned long long sync_dirty; /* one bit per port */
    atomic_uint clock_ms, sync_last_ms, sync_first_ms;
    atomic_int sync_now;               /* explicit Save: send without waiting */
    unsigned action_ms;                /* debounce for Save/Restore controls */
    double   clock_frac;
    GrpcConn gc;
    int      sync_proc;                /* sushi processor id of this instance, -1 = unknown */
    int      sync_pid[P_COUNT];
    float    sync_min[P_COUNT], sync_max[P_COUNT];
    uint32_t sync_retry_ms;
    LV2_URID_Map* map;
    LV2_URID u_chunk, u_int, k_version, k_bank, k_slot[MAX_SLOTS + 1];
    atomic_int force_reload;        /* set by state restore: reload the current slot */
    pthread_mutex_t file_lock;      /* recordings folder: restore vs. new recordings */
    double rate;

    /* playback */
    Sample* cur;
    Voice    voices[MAX_VOICES];
    uint32_t age_counter;
    float    bend;
    int      pedal;

    /* loader thread hand-over */
    _Atomic(Sample*) pending;
    _Atomic(Sample*) retired;
    atomic_int want_slot;
    atomic_int running;
    pthread_t thread;
    int thread_ok;
    char bundle[PATH_LEN];
    char rec_dir[PATH_LEN];
    char loaded_path[PATH_LEN];
    time_t loaded_mtime;
    off_t loaded_size;

    /* recording (buffers preallocated) */
    atomic_int rec_state;
    int      rec_prev_switch;
    float*   rec_l;
    float*   rec_r;
    uint32_t rec_cap, rec_pos, rec_last_loud, rec_silent;
    uint32_t rec_len;   /* valid frames at hand-off */
    int      rec_slot;
    float    pre_l[PREROLL], pre_r[PREROLL];
    uint32_t pre_pos;
    float    scr_l[SCRATCH], scr_r[SCRATCH];
    float    v_l[SCRATCH], v_r[SCRATCH];   /* voices, before the filter */
    uint32_t v_base;

    float    cut_smooth; /* Cutoff, smoothed (0..100 units) */
    int      osc_wave;   /* -1 = play the sample, 0..3 = built-in oscillator (slots 61-64) */
} KeySampler;

/* ------------------------------------------------------------------ WAV */

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }

static float sample_to_float(const uint8_t* p, int fmt, int bits)
{
    if (fmt == 3) {
        if (bits == 32) { float f; memcpy(&f, p, 4); return f; }
        if (bits == 64) { double d; memcpy(&d, p, 8); return (float)d; }
        return 0.f;
    }
    switch (bits) {
    case 8:  return ((int)p[0] - 128) / 128.f;
    case 16: return (int16_t)rd16(p) / 32768.f;
    case 24: { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return v / 2147483648.f; }
    case 32: return (int32_t)rd32(p) / 2147483648.f;
    }
    return 0.f;
}

static void free_sample(Sample* s)
{
    if (!s) return;
    free(s->l);
    free(s->r);
    free(s);
}

static Sample* new_sample(uint32_t n, double rate)
{
    Sample* s = calloc(1, sizeof(Sample));
    if (!s) return NULL;
    s->l = calloc(n + 4, sizeof(float));
    s->r = calloc(n + 4, sizeof(float));
    if (!s->l || !s->r) { free_sample(s); return NULL; }
    s->frames = n;
    s->rate = rate;
    s->loop_start = 0;
    s->loop_end = n;
    return s;
}

static Sample* load_wav(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44) { fclose(f); return NULL; }
    uint8_t* buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return NULL; }

    int fmt = 0, ch = 0, bits = 0; uint32_t sr = 0;
    const uint8_t* data = NULL; uint32_t data_len = 0;
    int have_loop = 0; uint32_t ls = 0, le = 0;
    long off = 12;
    while (off + 8 <= size) {
        const uint8_t* c = buf + off;
        uint32_t len = rd32(c + 4);
        const uint8_t* body = c + 8;
        long avail = size - (off + 8);
        if (len > (uint32_t)avail) len = (uint32_t)avail;
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); sr = rd32(body + 4); bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);
        } else if (!memcmp(c, "data", 4)) {
            data = body; data_len = len;
        } else if (!memcmp(c, "smpl", 4) && len >= 36 + 24 && rd32(body + 28) > 0) {
            ls = rd32(body + 36 + 8); le = rd32(body + 36 + 12); have_loop = 1;
        }
        off += 8 + len + (len & 1);
    }
    if (!data || ch < 1 || sr == 0 || (fmt != 1 && fmt != 3) ||
        !(bits == 8 || bits == 16 || bits == 24 || bits == 32 || (fmt == 3 && bits == 64))) {
        fprintf(stderr, "KeySampler: unsupported WAV %s (fmt %d, %d ch, %d bit)\n", path, fmt, ch, bits);
        free(buf); return NULL;
    }
    int bps = bits / 8, stride = bps * ch;
    uint32_t n = data_len / stride;
    Sample* s = n ? new_sample(n, sr) : NULL;
    if (!s) { free(buf); return NULL; }
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* fr = data + (size_t)i * stride;
        float l = sample_to_float(fr, fmt, bits);
        s->l[i + 1] = l;
        s->r[i + 1] = ch > 1 ? sample_to_float(fr + bps, fmt, bits) : l;
    }
    if (have_loop && le > ls && le < n) { s->loop_start = ls; s->loop_end = le + 1; }
    free(buf);
    fprintf(stderr, "KeySampler: loaded %s (%u frames, %u Hz, %d ch, %d bit)\n", path, n, sr, ch, bits);
    return s;
}


/* 32-bit float stereo WAV, written to a temp file then renamed (never half-written) */
static int write_file_atomic(const char* path, const void* data, size_t len)
{
    char tmp[PATH_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    fflush(f);
    fsync(fileno(f));
    ok = ok && !ferror(f);
    fclose(f);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* 16-bit stereo PCM WAV (same as the S2400's own files; half the size of float) */
static int save_wav(const char* path, const Sample* s)
{
    uint32_t n = s->frames, data = n * 4;
    uint8_t* buf = malloc(44 + (size_t)data);
    if (!buf) return -1;
    uint8_t* p = buf;
#define P32(v) do { uint32_t x_ = (v); memcpy(p, &x_, 4); p += 4; } while (0)
#define P16(v) do { uint16_t x_ = (v); memcpy(p, &x_, 2); p += 2; } while (0)
    memcpy(p, "RIFF", 4); p += 4; P32(36 + data); memcpy(p, "WAVEfmt ", 8); p += 8;
    P32(16); P16(1); P16(2); P32((uint32_t)s->rate); P32((uint32_t)s->rate * 4); P16(4); P16(16);
    memcpy(p, "data", 4); p += 4; P32(data);
    for (uint32_t i = 0; i < n; ++i) {
        float l = s->l[i + 1], r = s->r[i + 1];
        int16_t a = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, l)) * 32767.f);
        int16_t b = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, r)) * 32767.f);
        memcpy(p, &a, 2); memcpy(p + 2, &b, 2); p += 4;
    }
#undef P32
#undef P16
    int rc = write_file_atomic(path, buf, 44 + (size_t)data);
    free(buf);
    return rc;
}

static void* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* buf = n > 0 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* -------------------------------------------------------- file selection */

static int is_wav(const struct dirent* d)
{
    size_t L = strlen(d->d_name);
    return d->d_name[0] != '.' && L > 4 && !strcasecmp(d->d_name + L - 4, ".wav");
}

/* n-th (1-based) WAV by name in samples/ (or the bundle folder). 0 if none. */
static int nth_wav(const char* bundle, int n, char* out)
{
    const char* dirs[2] = { "samples/", "" };
    for (int d = 0; d < 2; ++d) {
        char dir[PATH_LEN];
        snprintf(dir, sizeof dir, "%s%s", bundle, dirs[d]);
        struct dirent** list = NULL;
        int cnt = scandir(dir, &list, is_wav, alphasort);
        if (cnt <= 0) { free(list); continue; }
        int found = 0;
        if (n >= 1 && n <= cnt && n <= MAX_FILES) {
            snprintf(out, PATH_LEN, "%s%.200s", dir, list[n - 1]->d_name);
            found = 1;
        }
        for (int i = 0; i < cnt; ++i) free(list[i]);
        free(list);
        return found;
    }
    return 0;
}

/* Bank 1 lives directly in the recordings folder (where v3 put its recordings),
 * other banks in their own sub-folder: bank02/slot01.wav ... */
static void bank_dir(const KeySampler* s, int bank, char* out)
{
    if (bank <= 1) snprintf(out, PATH_LEN, "%s", s->rec_dir);
    else snprintf(out, PATH_LEN, "%sbank%02d/", s->rec_dir, bank);
}

static void slot_path(const KeySampler* s, int key, char* out)
{
    char dir[PATH_LEN];
    bank_dir(s, key / 100, dir);
    snprintf(out, PATH_LEN, "%.900sslot%02d.wav", dir, key % 100);
}

/* File for a slot: its recording if there is one, else the n-th file in samples/ */
static int path_for_slot(const KeySampler* s, int key, char* out)
{
    struct stat st;
    slot_path(s, key, out);
    if (stat(out, &st) == 0) return 1;
    return nth_wav(s->bundle, key % 100, out);
}

static void remember_loaded(KeySampler* s, const char* path)
{
    struct stat st;
    snprintf(s->loaded_path, PATH_LEN, "%s", path);
    if (stat(path, &st) == 0) { s->loaded_mtime = st.st_mtime; s->loaded_size = st.st_size; }
}

/* Returns a newly loaded sample if the slot's file differs from what's loaded.
 * *empty is set when the slot has nothing at all. */
static Sample* check_selection(KeySampler* s, int* empty)
{
    char path[PATH_LEN];
    struct stat st;
    *empty = 0;
    if (!path_for_slot(s, atomic_load(&s->want_slot), path) || stat(path, &st) != 0) {
        *empty = s->loaded_path[0] != 0;
        s->loaded_path[0] = 0;
        return NULL;
    }
    if (!strcmp(path, s->loaded_path) && st.st_mtime == s->loaded_mtime && st.st_size == s->loaded_size)
        return NULL;
    remember_loaded(s, path);
    return load_wav(path);
}

/* An empty Sample (0 frames) tells the audio thread to go silent. */
static Sample* silent_sample(void) { return new_sample(0, 48000); }

static void handoff_recording(KeySampler* s)
{
    uint32_t n = s->rec_len;
    Sample* smp = new_sample(n, s->rate);
    if (smp) {
        memcpy(smp->l + 1, s->rec_l, n * sizeof(float));
        memcpy(smp->r + 1, s->rec_r, n * sizeof(float));
    }
    atomic_store(&s->rec_state, REC_IDLE); /* buffer is free again */
    if (!smp) return;

    char path[PATH_LEN];
    slot_path(s, s->rec_slot, path);
    char dir[PATH_LEN];
    mkdir(s->rec_dir, 0755);
    bank_dir(s, s->rec_slot / 100, dir);
    mkdir(dir, 0755);
    pthread_mutex_lock(&s->file_lock);
    int saved = save_wav(path, smp);
    pthread_mutex_unlock(&s->file_lock);
    if (saved == 0) {
        fprintf(stderr, "KeySampler: recorded %u frames into %s\n", n, path);
        if (atomic_load(&s->want_slot) == s->rec_slot) remember_loaded(s, path);
    } else {
        fprintf(stderr, "KeySampler: recorded %u frames (could not save %s: %s)\n", n, path, strerror(errno));
    }
    if (atomic_load(&s->want_slot) == s->rec_slot) atomic_store(&s->pending, smp);
    else free_sample(smp);
}

static void cc_reset(KeySampler* s);

static void cc_map_path(const KeySampler* s, char* out)
{
    snprintf(out, PATH_LEN, "%.1000scc_map.txt", s->rec_dir);
}

static int cc_default(int cc);

/* Only changes from the defaults are stored ("<cc> 0" = cleared), so new
 * default assignments in later versions still show up. CC 128 = aftertouch. */
static void cc_map_save(KeySampler* s)
{
    char path[PATH_LEN], buf[4096];
    int n = snprintf(buf, sizeof buf, "# KeySampler MIDI CC map v2: <cc> <target>, differences from defaults\n");
    for (int i = 0; i < CC_SLOTS; ++i)
        if (s->cc_map[i] != cc_default(i))
            n += snprintf(buf + n, sizeof buf - n, "%d %d\n", i, s->cc_map[i]);
    mkdir(s->rec_dir, 0755);
    cc_map_path(s, path);
    write_file_atomic(path, buf, (size_t)n);
}

static void cc_map_load(KeySampler* s)
{
    char path[PATH_LEN], line[128];
    cc_reset(s);
    cc_map_path(s, path);
    FILE* f = fopen(path, "r");
    if (!f) return;
    if (!fgets(line, sizeof line, f) || !strstr(line, "v2")) { fclose(f); return; } /* older file: use defaults */
    while (fgets(line, sizeof line, f)) {
        int cc, t;
        if (line[0] != '#' && sscanf(line, "%d %d", &cc, &t) == 2 && cc >= 0 && cc < CC_SLOTS && t >= 0 && t < T_COUNT)
            s->cc_map[cc] = (int8_t)t;
    }
    fclose(f);
}

static const char* const port_sym[P_COUNT] = {
    "events", "audio_out_l", "audio_out_r", "audio_in_l", "audio_in_r",
    "slot", "bank", "record", "threshold", "mode", "level", "root", "tune",
    "attack", "decay", "sustain", "release",
    "start_sec", "start_msec", "loop_start_sec", "loop_start_msec", "loop_end_sec", "loop_end_msec",
    "filter", "cutoff", "resonance",
    "fenv_amount", "fenv_attack", "fenv_decay", "fenv_sustain", "fenv_release",
    "glide_mode", "glide", "vib_depth", "pan", "voices", "cc_learn",
    "thru", "vib_rate", "velsens", "bend", "vib_delay",
    "start_smp", "loop_start_smp", "loop_end_smp", "fenv", "menu_sync",
};

static float port_raw(const KeySampler* s, int p);

/* ---------- menu sync (loader thread) */

static int sync_identify(KeySampler* s)
{
    static uint8_t buf[60000];
    int n = grpc_call(&s->gc, "/sushi_rpc.AudioGraphController/GetAllProcessors", NULL, 0, buf, sizeof buf);
    if (n < 0) return -1;
    int cand[32], ncand = 0;
    size_t pos = 0;
    PbField f;
    while (ncand < 32 && pb_next(buf, (size_t)n, &pos, &f) == 0) {
        if (f.field != 1 || !f.sub) continue;
        size_t q = 0; PbField g;
        int id = 0, count = 0, named = 0; /* proto3: absent field = 0 */
        while (pb_next(f.sub, f.sublen, &q, &g) == 0) {
            if (g.field == 1 && g.wire == 0) id = (int)g.v;
            else if ((g.field == 2 || g.field == 3) && g.sub) {
                char t[128]; size_t L = g.sublen < 127 ? g.sublen : 127;
                memcpy(t, g.sub, L); t[L] = 0;
                if (strcasestr(t, "keysampler")) named = 1;
            } else if (g.field == 4 && g.wire == 0) count = (int)g.v;
        }
        if (id >= 0 && named && count == P_COUNT - 5) cand[ncand++] = id;
    }

    int best = -1, best_score = -1, ties = 0;
    int pid[P_COUNT]; float mn[P_COUNT], mx[P_COUNT];
    for (int c = 0; c < ncand; ++c) {
        uint8_t req[16];
        size_t rl = 0; req[rl++] = 0x08; rl += pb_varint(req + rl, (uint64_t)(uint32_t)cand[c]);
        n = grpc_call(&s->gc, "/sushi_rpc.ParameterController/GetProcessorParameters", req, (uint32_t)rl, buf, sizeof buf);
        if (getenv("KEYSAMPLER_DEBUG")) fprintf(stderr, "KeySampler: GetProcessorParameters(%d) -> %d bytes\n", cand[c], n);
        if (n < 0) continue;
        int cpid[P_COUNT]; float cmn[P_COUNT], cmx[P_COUNT];
        for (int i = 0; i < P_COUNT; ++i) { cpid[i] = -1; cmn[i] = 0; cmx[i] = 1; }
        pos = 0;
        while (pb_next(buf, (size_t)n, &pos, &f) == 0) {
            if (f.field != 1 || !f.sub) continue;
            size_t q = 0; PbField g;
            int id = 0; char nm[64] = "", lb[64] = ""; float lo = 0, hi = 0; /* proto3 defaults */
            while (pb_next(f.sub, f.sublen, &q, &g) == 0) {
                if (g.field == 1 && g.wire == 0) id = (int)g.v;
                else if (g.field == 3 && g.sub) { size_t L = g.sublen < 63 ? g.sublen : 63; memcpy(lb, g.sub, L); lb[L] = 0; }
                else if (g.field == 4 && g.sub) { size_t L = g.sublen < 63 ? g.sublen : 63; memcpy(nm, g.sub, L); nm[L] = 0; }
                else if (g.field == 7 && g.wire == 5) lo = g.f;
                else if (g.field == 8 && g.wire == 5) hi = g.f;
            }
            for (int pi = P_FIRST_CTL; pi < P_COUNT; ++pi)
                if (!strcmp(nm, port_sym[pi]) || !strcmp(lb, port_sym[pi])) { cpid[pi] = id; cmn[pi] = lo; cmx[pi] = hi; }
        }
        /* score: how many of this processor's values equal our own menu values */
        int score = 0;
        for (int pi = P_FIRST_CTL; pi < P_COUNT; ++pi) {
            if (pi == P_LEARN || pi == P_RECORD) continue;
            if (cpid[pi] < 0) { fprintf(stderr, "KeySampler: menu sync: sushi has no parameter \"%s\"\n", port_sym[pi]); score = -1000; break; }
            if (ncand == 1) continue;
            uint8_t r2[24]; size_t l2 = pb_param_id(r2, cand[c], cpid[pi]);
            uint8_t out[16];
            int m = grpc_call(&s->gc, "/sushi_rpc.ParameterController/GetParameterValue", r2, (uint32_t)l2, out, sizeof out);
            float val = 0; size_t q = 0; PbField g;
            while (m > 0 && pb_next(out, (size_t)m, &q, &g) == 0) if (g.field == 1 && g.wire == 5) val = g.f;
            float range = cmx[pi] - cmn[pi];
            float mine = range != 0 ? (port_raw(s, pi) - cmn[pi]) / range : 0;
            if (fabsf(mine - val) < 0.002f) ++score;
        }
        if (score > best_score) { best_score = score; best = c; ties = 0;
            memcpy(pid, cpid, sizeof pid); memcpy(mn, cmn, sizeof mn); memcpy(mx, cmx, sizeof mx); }
        else if (score == best_score) ++ties;
    }
    if (best < 0 || best_score < 0 || ties) {
        fprintf(stderr, "KeySampler: menu sync: could not identify this instance (%d candidates)\n", ncand);
        return -1;
    }
    s->sync_proc = cand[best];
    memcpy(s->sync_pid, pid, sizeof pid); memcpy(s->sync_min, mn, sizeof mn); memcpy(s->sync_max, mx, sizeof mx);
    fprintf(stderr, "KeySampler: menu sync: this instance is sushi processor %d\n", s->sync_proc);
    return 0;
}

static void menu_sync_step(KeySampler* s)
{
    if (!atomic_load(&s->sync_dirty)) return;
    unsigned now = atomic_load(&s->clock_ms);
    if (!atomic_exchange(&s->sync_now, 0)) {
        /* automatic sync: wait until the control has rested 120 ms (at most 400 ms) */
        if (port_raw(s, P_MENU_SYNC) < 0.5f) { atomic_store(&s->sync_dirty, 0); return; }
        if (now - atomic_load(&s->sync_last_ms) < 120 && now - atomic_load(&s->sync_first_ms) < 400) return;
    }
    unsigned long long bits = atomic_exchange(&s->sync_dirty, 0);
    if (s->sync_proc < 0) {
        if (now < s->sync_retry_ms) return;
        if (sync_identify(s)) { s->sync_retry_ms = now + 5000; grpc_close(&s->gc); return; }
    }
    for (int p = P_FIRST_CTL; p < P_COUNT; ++p) {
        if (!(bits & (1ULL << p)) || s->sync_pid[p] < 0) continue;
        float range = s->sync_max[p] - s->sync_min[p];
        float norm = range != 0 ? (s->sync_val[p] - s->sync_min[p]) / range : 0;
        norm = norm < 0 ? 0 : norm > 1 ? 1 : norm;
        uint8_t req[48];
        size_t n = 0;
        uint8_t id[24]; size_t il = pb_param_id(id, s->sync_proc, s->sync_pid[p]);
        req[n++] = 0x0A; req[n++] = (uint8_t)il; memcpy(req + n, id, il); n += il;
        req[n++] = 0x15; memcpy(req + n, &norm, 4); n += 4;
        uint8_t out[16];
        if (grpc_call(&s->gc, "/sushi_rpc.ParameterController/SetParameterValue", req, (uint32_t)n, out, sizeof out) < 0) {
            s->sync_proc = -1; /* identify again next time */
            grpc_close(&s->gc);
            return;
        }
    }
}

static void* loader_thread(void* arg)
{
    KeySampler* s = arg;
    int tick = 0, last_slot = atomic_load(&s->want_slot);
    while (atomic_load(&s->running)) {
        free_sample(atomic_exchange(&s->retired, NULL));
        int idle = atomic_load(&s->pending) == NULL && atomic_load(&s->retired) == NULL;

        if (idle && atomic_load(&s->rec_state) == REC_HANDOFF) {
            handoff_recording(s);
        } else if (idle) {
            int slot = atomic_load(&s->want_slot);
            if (atomic_exchange(&s->map_dirty, 0)) cc_map_save(s);
            menu_sync_step(s);
            int forced = atomic_exchange(&s->force_reload, 0);
            if (forced) s->loaded_path[0] = 0;
            if (forced || slot != last_slot || tick % 40 == 0) { /* slot change: now; rescan: every 2 s */
                last_slot = slot;
                int empty;
                Sample* n = check_selection(s, &empty);
                if (!n && empty) n = silent_sample();
                if (n) atomic_store(&s->pending, n);
            }
        }
        usleep(atomic_load(&s->rec_state) == REC_HANDOFF ? 5000 : 50000);
        ++tick;
    }
    return NULL;
}

/* ------------------------------------------------------------------ LV2 */

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* features)
{
    (void)d;
    LV2_URID_Map* map = NULL;
    for (int i = 0; features && features[i]; ++i)
        if (!strcmp(features[i]->URI, LV2_URID__map)) map = (LV2_URID_Map*)features[i]->data;
    if (!map) return NULL;

    KeySampler* s = calloc(1, sizeof(KeySampler));
    if (!s) return NULL;
    s->rate = rate;
    s->midi_event = map->map(map->handle, LV2_MIDI__MidiEvent);
    s->map = map;
    s->u_chunk = map->map(map->handle, LV2_ATOM__Chunk);
    s->u_int = map->map(map->handle, LV2_ATOM__Int);
    s->k_version = map->map(map->handle, KEYSAMPLER_URI "#stateVersion");
    s->k_bank = map->map(map->handle, KEYSAMPLER_URI "#bank");
    for (int i = 1; i <= MAX_SLOTS; ++i) {
        char u[256];
        snprintf(u, sizeof u, KEYSAMPLER_URI "#slot%02d", i);
        s->k_slot[i] = map->map(map->handle, u);
    }
    pthread_mutex_init(&s->file_lock, NULL);
    atomic_init(&s->force_reload, 0);
    snprintf(s->bundle, PATH_LEN, "%s", bundle);
    /* <user>/plugins/lv2/keysampler.lv2/  ->  <user>/keysampler_recordings/
     * kept outside the plugin folder so Sync Files never touches it */
    snprintf(s->rec_dir, PATH_LEN, "%s../../../keysampler_recordings/", bundle);

    s->rec_cap = (uint32_t)(rate * REC_SECONDS);
    s->rec_l = calloc(s->rec_cap, sizeof(float));
    s->rec_r = calloc(s->rec_cap, sizeof(float));
    if (!s->rec_l || !s->rec_r) { free(s->rec_l); free(s->rec_r); free(s); return NULL; }

    atomic_init(&s->pending, NULL);
    atomic_init(&s->retired, NULL);
    atomic_init(&s->want_slot, KEY(1, 1));
    atomic_init(&s->running, 1);
    atomic_init(&s->rec_state, REC_IDLE);

    int empty;
    s->cur = check_selection(s, &empty); /* first sound ready before audio starts */
    for (int t = 1; t < T_COUNT; ++t) if (t_port[t] >= 0) s->port_target[t_port[t]] = (int8_t)t;
    cc_map_load(s);
    pthread_once(&g_wt_once, build_wavetables);
    s->osc_wave = -1;
    s->cut_smooth = -1.f;
    s->rpn = 0x3FFF;
    s->rpn_fine_msb = 64;
    s->last_program = -1;
    s->glide_on_mode = 1;
    s->expr = 1.f;
    s->gc.fd = -1;
    snprintf(s->gc.host, sizeof s->gc.host, "%s", getenv("KEYSAMPLER_GRPC_HOST") ? getenv("KEYSAMPLER_GRPC_HOST") : "127.0.0.1");
    s->gc.port = getenv("KEYSAMPLER_GRPC_PORT") ? atoi(getenv("KEYSAMPLER_GRPC_PORT")) : 51051;
    s->sync_proc = -1;
    atomic_init(&s->sync_dirty, 0);
    atomic_init(&s->sync_now, 0);
    atomic_init(&s->clock_ms, 0);
    atomic_init(&s->map_dirty, 0);
    s->thread_ok = pthread_create(&s->thread, NULL, loader_thread, s) == 0;
    return s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
    KeySampler* s = h;
    switch (port) {
    case P_MIDI_IN: s->midi_in = data; break;
    case P_OUT_L: s->out_l = data; break;
    case P_OUT_R: s->out_r = data; break;
    case P_IN_L: s->in_l = data; break;
    case P_IN_R: s->in_r = data; break;
    default: if (port < P_COUNT) s->ctl[port] = data;
    }
}

/* CC value (0..1) -> parameter value, with musical curves */

static float cc_scale(int t, float v)
{
    switch (t) {
    case T_LEVEL:   return v;                        /* 0..1 (full = unity gain) */
    case T_CUTOFF:  return v * 100.f;
    case T_RESO:    return v;
    case T_ATTACK:  return v * v * 2000.f;           /* fine control at short times */
    case T_DECAY:   return v * v * 5000.f;
    case T_SUSTAIN: return v;
    case T_RELEASE: return v * v * 5000.f;
    case T_TUNE:    return (v * 127.f - 64.f) / 64.f * 12.f; /* centre = 0, +-1 octave */
    case T_SLOT:    return 1.f + floorf(v * 63.f + 0.5f);
    case T_RECORD:  return v >= 0.5f ? 1.f : 0.f;
    case T_FILTER:  return floorf(v * 3.999f);
    case T_MODE:    return floorf(v * 2.999f);
    case T_PAN:     { float x = (v * 127.f - 64.f) / 63.f; return x < -1 ? -1 : x > 1 ? 1 : x; }
    case T_VIB_RATE:  return 0.1f + v * v * 11.9f;
    case T_VIB_DEPTH: return v * 100.f;
    case T_VIB_DELAY: return v * 2000.f;
    case T_THRU:    return v >= 0.5f ? 1.f : 0.f;
    case T_GLIDE_MODE: return floorf(v * 2.999f);
    case T_GLIDE:   return v * v * 2000.f;
    case T_FENV_ON: return v >= 0.5f ? 1.f : 0.f;
    case T_FENV_AMT: { float x = (v * 127.f - 64.f) / 63.f * 100.f; return x < -100 ? -100 : x > 100 ? 100 : x; }
    case T_FENV_A:  return v * v * 2000.f;
    case T_FENV_D:  return v * v * 5000.f;
    case T_FENV_S:  return v;
    case T_FENV_R:  return v * v * 5000.f;
    }
    return 0.f;
}

static float ctl(const KeySampler* s, int p, float def, float lo, float hi)
{
    float v = s->ctl[p] ? *s->ctl[p] : def;
    if (!(v == v)) v = def;
    int t = s->port_target[p];
    if (t && s->ov_on[t] && t != T_START && t != T_LOOP_START && t != T_LOOP_END)
        v = (t == T_TUNE && s->tune_is_rpn) ? s->rpn_coarse + s->rpn_fine / 100.f : cc_scale(t, s->ov[t]);
    return v < lo ? lo : v > hi ? hi : v;
}

static float port_raw(const KeySampler* s, int p) { return s->ctl[p] ? *s->ctl[p] : 0.f; }

/* the menu value(s) behind a target, to notice when the user changes them */
static double target_snapshot(const KeySampler* s, int t)
{
    int p = t_port[t];
    if (p < 0) return 0;
    if (t == T_START || t == T_LOOP_START || t == T_LOOP_END)
        return port_raw(s, p) * 1e6 + port_raw(s, point_ms(p)) * 1e3 + port_raw(s, point_smp(p));
    return port_raw(s, p);
}

static int cc_default(int cc)
{
    for (size_t i = 0; i < sizeof cc_defaults / sizeof cc_defaults[0]; ++i)
        if (cc_defaults[i].cc == cc) return cc_defaults[i].t;
    return T_NONE;
}

/* audio thread: remember the value(s) the menu should show for target t */
static void queue_sync(KeySampler* s, int t)
{
    int p = t_port[t];
    unsigned long long bits = 0;
    if (t == T_START || t == T_LOOP_START || t == T_LOOP_END) {
        const Sample* sm = s->cur;
        if (!sm || !sm->frames) return;
        double f = s->ov[t] * (sm->frames - 1);
        double total_ms = f * 1000.0 / sm->rate;
        if (t == T_LOOP_END && f >= sm->frames - 1.5) total_ms = f = 0; /* = "end of sound" */
        double sec = floor(total_ms / 1000.0);
        if (sec > MAX_SEC) sec = MAX_SEC;
        double ms = floor(total_ms - sec * 1000.0);
        if (ms > 999) ms = 999;
        double smp = floor(f - (sec * 1000.0 + ms) * sm->rate / 1000.0 + 0.5);
        if (smp < 0) smp = 0;
        if (smp > MAX_SMP) smp = MAX_SMP;
        s->sync_val[p] = (float)sec; s->sync_val[point_ms(p)] = (float)ms; s->sync_val[point_smp(p)] = (float)smp;
        bits = (1ULL << p) | (1ULL << point_ms(p)) | (1ULL << point_smp(p));
    } else if (t == T_TUNE && s->tune_is_rpn) {
        float v = s->rpn_coarse + s->rpn_fine / 100.f;
        s->sync_val[p] = v < -24 ? -24 : v > 24 ? 24 : v;
        bits = 1ULL << p;
    } else {
        s->sync_val[p] = cc_scale(t, s->ov[t]);
        bits = 1ULL << p;
    }
    unsigned now = atomic_load(&s->clock_ms);
    if (!atomic_load(&s->sync_dirty)) atomic_store(&s->sync_first_ms, now);
    atomic_store(&s->sync_last_ms, now);
    atomic_fetch_or(&s->sync_dirty, bits);
}

static void set_override(KeySampler* s, int t, float v)
{
    if (!s->ov_on[t]) s->ov_snap[t] = target_snapshot(s, t);
    s->ov[t] = v < 0 ? 0 : v > 1 ? 1 : v;
    s->ov_on[t] = 1;
    if (port_raw(s, P_MENU_SYNC) > 0.5f) queue_sync(s, t); /* automatic sync (off by default) */
}

/* B5: write every value changed from a controller into the menu */
static void save_to_menu(KeySampler* s)
{
    for (int t = 1; t < T_COUNT; ++t)
        if (t_port[t] >= 0 && s->ov_on[t]) queue_sync(s, t);
    atomic_store(&s->sync_now, 1);
}

/* B6: forget controller changes, play the menu values again */
static void restore_menu(KeySampler* s)
{
    for (int t = 1; t < T_COUNT; ++t) s->ov_on[t] = 0;
    s->tune_is_rpn = 0;
    atomic_store(&s->sync_dirty, 0);
}

static void cc_reset(KeySampler* s)
{
    memset(s->cc_map, 0, sizeof s->cc_map);
    for (size_t i = 0; i < sizeof cc_defaults / sizeof cc_defaults[0]; ++i)
        s->cc_map[cc_defaults[i].cc] = (int8_t)cc_defaults[i].t;
}

static void handle_cc(KeySampler* s, int cc, int val)
{
    if (s->learn_armed && s->learn_prev > T_NONE && s->learn_prev < T_COUNT) {
        for (int i = 0; i < CC_SLOTS; ++i)
            if (s->cc_map[i] == s->learn_prev) s->cc_map[i] = 0; /* one CC per target */
        s->cc_map[cc] = (int8_t)s->learn_prev;
        s->learn_armed = 0;
        atomic_store(&s->map_dirty, 1);
    }
    int t = s->cc_map[cc];
    if (t <= T_NONE || t >= T_COUNT) return;
    if (t == T_SAVE || t == T_RESTORE) { /* learned action buttons: any message, 0.2 s debounce */
        unsigned now = atomic_load(&s->clock_ms);
        if (now - s->action_ms < 200) return;
        s->action_ms = now;
        if (t == T_SAVE) save_to_menu(s); else restore_menu(s);
        return;
    }
    if (t == T_TUNE) s->tune_is_rpn = 0;
    set_override(s, t, val / 127.f);
}

/* PCR buttons B1-B6: toggle / step through values */
static void cycle_filter(KeySampler* s)
{
    int f = ((int)(ctl(s, P_FILTER, 0, 0, 3) + 0.5f) + 1) % 4; /* Off > LP > BP > HP > Off */
    set_override(s, T_FILTER, (f + 0.5f) / 4.f);
}
static void cycle_mode(KeySampler* s)
{
    int m = ((int)(ctl(s, P_MODE, 1, 0, 2) + 0.5f) + 1) % 3;   /* One Shot > Gate > Loop */
    set_override(s, T_MODE, (m + 0.5f) / 3.f);
}
static void toggle_record(KeySampler* s)
{
    set_override(s, T_RECORD, ctl(s, P_RECORD, 0, 0, 1) > 0.5f ? 0.f : 1.f);
}
static void toggle_glide(KeySampler* s)
{
    int gm = (int)(ctl(s, P_GLIDE_MODE, 0, 0, 2) + 0.5f);
    if (gm) s->glide_on_mode = gm;
    int next = gm ? 0 : (s->glide_on_mode ? s->glide_on_mode : 1);
    set_override(s, T_GLIDE_MODE, (next + 0.5f) / 3.f);
}

/* RPN: CC101/100 select, CC6/38 data. 0/2 = coarse tune, 0/1 = fine tune (PCR R6/R7) */
static void handle_rpn_data(KeySampler* s, int cc, int val)
{
    if (s->rpn == 2 && cc == 6) s->rpn_coarse = (float)(val - 64);
    else if (s->rpn == 1 && cc == 6) { s->rpn_fine_msb = val; s->rpn_fine = ((val << 7) - 8192) / 8192.f * 100.f; }
    else if (s->rpn == 1 && cc == 38) s->rpn_fine = (((s->rpn_fine_msb << 7) | val) - 8192) / 8192.f * 100.f;
    else return;
    s->tune_is_rpn = 1;
    set_override(s, T_TUNE, 0.5f);
}

static float ms_to_step(float ms, double rate) { return ms <= 0.05f ? 1.f : (float)(1000.0 / (ms * rate)); }

static void release_voice(KeySampler* s, Voice* v)
{
    if (v->stage == ENV_OFF || v->stage == ENV_RELEASE) return;
    v->rel_step = v->env * ms_to_step(ctl(s, P_RELEASE, 150, 0, 10000), s->rate);
    if (v->rel_step <= 0) v->rel_step = 1;
    v->stage = ENV_RELEASE;
    v->frel_step = v->fenv * ms_to_step(ctl(s, P_FENV_R, 300, 0, 10000), s->rate);
    v->fstage = ENV_RELEASE;
}

/* ms + samples -> frame position in the sample (samples at the sample's own rate) */
static double point_frames(const KeySampler* s, const Sample* sm, int p_s)
{
    int t = s->port_target[p_s];
    if (t && s->ov_on[t]) {
        double f = s->ov[t] * (sm->frames > 0 ? sm->frames - 1 : 0);
        return (t == T_LOOP_END && f < 1.0) ? 1.0 : f; /* 0 would mean "end" */
    }
    double sec = floor(ctl(s, p_s, 0, 0, MAX_SEC) + 0.5);
    double ms = floor(ctl(s, point_ms(p_s), 0, 0, 999) + 0.5);
    double smp = floor(ctl(s, point_smp(p_s), 0, 0, MAX_SMP) + 0.5);
    return (sec * 1000.0 + ms) * sm->rate / 1000.0 + smp;
}

enum { GLIDE_OFF = 0, GLIDE_PORTA = 1, GLIDE_LEGATO = 2 };

static void held_push(KeySampler* s, int note)
{
    int j = 0;
    for (int i = 0; i < s->n_held; ++i) if (s->held_keys[i] != note) s->held_keys[j++] = s->held_keys[i];
    s->n_held = j;
    if (s->n_held < 32) s->held_keys[s->n_held++] = note;
}

static void held_remove(KeySampler* s, int note)
{
    int j = 0;
    for (int i = 0; i < s->n_held; ++i) if (s->held_keys[i] != note) s->held_keys[j++] = s->held_keys[i];
    s->n_held = j;
}

/* start a glide from v->pitch to note over Glide ms (0 = jump) */
static void glide_to(KeySampler* s, Voice* v, int note)
{
    float ms = ctl(s, P_GLIDE, 0, 0, 2000);
    v->target = note;
    if (ms <= 0.f) { v->pitch = note; v->glide_step = 0; return; }
    v->glide_step = fabs(v->target - v->pitch) / (ms * s->rate / 1000.0);
}

static void note_on(KeySampler* s, int note, int vel)
{
    if (s->osc_wave < 0 && (!s->cur || !s->cur->frames)) return;
    int gm = (int)(ctl(s, P_GLIDE_MODE, 0, 0, 2) + 0.5f);
    int maxv = gm == GLIDE_LEGATO ? 1 : (int)ctl(s, P_VOICES, MAX_VOICES, 1, MAX_VOICES);
    int legato_held = s->n_held > 0;
    held_push(s, note);

    if (gm == GLIDE_LEGATO && legato_held) {
        /* a key is still down: keep the sound and envelope going, just move the pitch */
        Voice* v = &s->voices[0];
        if (v->stage != ENV_OFF && v->stage != ENV_RELEASE) {
            v->note = note;
            v->held = 1;
            v->sustained = 0;
            glide_to(s, v, note);
            s->last_pitch = note; s->have_last = 1;
            return;
        }
    }

    Voice* v = NULL;
    for (int i = 0; i < maxv; ++i)
        if (s->voices[i].stage != ENV_OFF && s->voices[i].note == note) v = &s->voices[i];
    if (!v) for (int i = 0; i < maxv; ++i) if (s->voices[i].stage == ENV_OFF) { v = &s->voices[i]; break; }
    if (!v) {
        float best = 1e30f;
        for (int i = 0; i < maxv; ++i) {
            Voice* c = &s->voices[i];
            float score = (c->stage == ENV_RELEASE ? 0.f : 1e9f) - (float)(s->age_counter - c->age);
            if (score < best) { best = score; v = c; }
        }
    }
    for (int i = maxv; i < MAX_VOICES; ++i) release_voice(s, &s->voices[i]);

    float vs = ctl(s, P_VELSENS, 1, 0, 1), vn = vel / 127.f;
    v->vel_gain = (1.f - vs) + vs * vn * vn;
    /* Portamento: every note slides from the previous note's pitch.
     * In a mono voice that is still sounding, start from where it is now. */
    double from = note;
    if (gm == GLIDE_PORTA && s->have_last)
        from = (maxv == 1 && v->stage != ENV_OFF) ? v->pitch : s->last_pitch;
    v->pitch = from;
    glide_to(s, v, note);
    if (gm != GLIDE_PORTA) { v->pitch = note; v->glide_step = 0; }
    v->note = note;
    v->held = 1;
    v->sustained = 0;
    if (s->osc_wave < 0) {
        v->pos = point_frames(s, s->cur, P_START_S);
        if (v->pos > s->cur->frames - 1) v->pos = s->cur->frames - 1;
    } else {
        v->pos = 0;
    }
    v->phase = 0;
    v->fic1[0] = v->fic1[1] = v->fic2[0] = v->fic2[1] = 0.f;
    v->fenv = 0.f;
    v->fstage = ENV_ATTACK;
    v->fcount = 0;
    v->env = 0.f;
    v->stage = ENV_ATTACK;
    v->age = ++s->age_counter;
    v->vib_t = 0;
    s->last_pitch = note; s->have_last = 1;
}

static void note_off(KeySampler* s, int note)
{
    int mode = (int)ctl(s, P_MODE, MODE_GATE, 0, 2);
    int gm = (int)(ctl(s, P_GLIDE_MODE, 0, 0, 2) + 0.5f);
    held_remove(s, note);
    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice* v = &s->voices[i];
        if (v->stage == ENV_OFF || v->note != note || !v->held) continue;
        if (gm == GLIDE_LEGATO && s->n_held > 0 && v->stage != ENV_RELEASE) {
            /* legato: fall back to the most recent key still held */
            int back = s->held_keys[s->n_held - 1];
            v->note = back;
            glide_to(s, v, back);
            s->last_pitch = back;
            continue;
        }
        v->held = 0;
        if (mode == MODE_ONESHOT && s->osc_wave < 0) continue; /* oscillators always stop on release */
        if (s->pedal) v->sustained = 1;
        else release_voice(s, v);
    }
}

static void handle_midi(KeySampler* s, const uint8_t* m, uint32_t size)
{
    if (size < 1) return;
    uint8_t st = m[0] & 0xF0;
    if (st == 0x90 && size >= 3 && m[2] > 0) note_on(s, m[1], m[2]);
    else if ((st == 0x80 || st == 0x90) && size >= 3) note_off(s, m[1]);
    else if (st == 0xE0 && size >= 3) s->bend = (((m[2] << 7) | m[1]) - 8192) / 8192.f;
    else if (st == 0xB0 && size >= 3) {
        if (m[1] == 64) {
            s->pedal = m[2] >= 64;
            if (!s->pedal)
                for (int i = 0; i < MAX_VOICES; ++i)
                    if (s->voices[i].sustained) { s->voices[i].sustained = 0; release_voice(s, &s->voices[i]); }
        } else if (m[1] == 120) {
            for (int i = 0; i < MAX_VOICES; ++i) s->voices[i].stage = ENV_OFF;
            s->n_held = 0;
        } else if (m[1] == 123) {
            s->n_held = 0;
            for (int i = 0; i < MAX_VOICES; ++i) { s->voices[i].held = 0; release_voice(s, &s->voices[i]); }
        } else if (m[1] == 101) { s->rpn = (s->rpn & 0x7F) | (m[2] << 7);
        } else if (m[1] == 100) { s->rpn = (s->rpn & ~0x7F) | m[2];
        } else if (m[1] == 6 || m[1] == 38) { handle_rpn_data(s, m[1], m[2]);
        } else if (m[1] == 11) { s->expr = m[2] / 127.f;              /* P2 expression */
        } else if (m[1] == 65) { cycle_mode(s);                       /* B3 "portamento on/off" */
        } else if (m[1] == 127) { toggle_glide(s);                    /* B4 "poly mode on" */
        } else if (m[1] == 126) { save_to_menu(s);                    /* B5 "mono mode on" */
        } else if (m[1] < 120 && m[1] != 98 && m[1] != 99) {
            handle_cc(s, m[1], m[2] & 0x7F);
        }
    } else if (st == 0xD0 && size >= 2) {                              /* S2 aftertouch */
        handle_cc(s, CC_PRESSURE, m[1] & 0x7F);
    } else if (st == 0xC0 && size >= 2) {      /* B1 = program -, B2 = program +: told apart by direction */
        int pgm = m[1] & 0x7F;
        int dir = s->last_program < 0 ? (pgm == 0 ? -1 : 1)
                : pgm < s->last_program ? -1 : pgm > s->last_program ? 1
                : (pgm == 0 ? -1 : 1);                                 /* repeated min/max */
        s->last_program = pgm;
        if (dir < 0) toggle_record(s);   /* B1 */
        else cycle_filter(s);            /* B2 */
    } else if (m[0] == 0xF0 && size >= 6 && m[1] == 0x7E && m[3] == 0x09 && m[4] == 0x03) {
        restore_menu(s);                                               /* B6 "GM2 system on" */
    }
}

static inline float hermite(const float* d, uint32_t i, float t)
{
    float xm1 = d[i], x0 = d[i + 1], x1 = d[i + 2], x2 = d[i + 3];
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

static void render(KeySampler* s, uint32_t from, uint32_t to)
{
    const Sample* sm = s->cur;
    int osc = s->osc_wave;
    if (from >= to) return;
    if (osc < 0 && (!sm || !sm->frames)) return;
    if (osc >= 0 && !g_wt) return;

    float level = ctl(s, P_LEVEL, 1, 0, 2);
    float root = ctl(s, P_ROOT, 60, 0, 127);
    float tune = ctl(s, P_TUNE, 0, -24, 24);
    float bend_r = ctl(s, P_BEND, 2, 0, 24);
    float a_step = ms_to_step(ctl(s, P_ATTACK, 1, 0, 10000), s->rate);
    float d_ms = ctl(s, P_DECAY, 200, 0, 10000);
    float sus = ctl(s, P_SUSTAIN, 1, 0, 1);
    int mode = (int)ctl(s, P_MODE, MODE_GATE, 0, 2);
    if (osc >= 0) mode = MODE_GATE; /* oscillators always sustain while held */
    float vib_rate = ctl(s, P_VIB_RATE, 5, 0.1f, 12);
    float vib_depth = ctl(s, P_VIB_DEPTH, 0, 0, 100);
    double vib_delay = ctl(s, P_VIB_DELAY, 0, 0, 2000) / 1000.0;

    /* filter: per voice, cutoff = smoothed Cutoff + envelope x Amount */
    int ftype = (int)(ctl(s, P_FILTER, 0, 0, 3) + 0.5f);
    float cut_target = ctl(s, P_CUTOFF, 100, 0, 100);
    if (s->cut_smooth < 0) s->cut_smooth = cut_target;
    s->cut_smooth += (cut_target - s->cut_smooth) * (float)(1.0 - exp(-(double)(to - from) / (0.01 * s->rate)));
    float fk = 1.414f - 1.36f * ctl(s, P_RESO, 0, 0, 1);
    int fenv_on = ctl(s, P_FENV_ON, 1, 0, 1) > 0.5f;
    float famt = ctl(s, P_FENV_AMT, 0, -100, 100); /* 0 = no filter envelope */
    float fa_step = ms_to_step(ctl(s, P_FENV_A, 5, 0, 10000), s->rate);
    float fsus = ctl(s, P_FENV_S, 0.2f, 0, 1);
    float fd_step = (1.f - fsus) * ms_to_step(ctl(s, P_FENV_D, 300, 0, 10000), s->rate);
    float nyq = (float)(0.45 * s->rate);

    double base = sm ? sm->rate / s->rate : 1.0;
    uint32_t ls = 0, le = 0;
    if (osc < 0) {
        /* Loop Start/End in sec+ms+samples. Loop End 0:00.000+0 means "end of the
         * sound". With both points at 0 the WAV's own loop points (if any) are used. */
        double lsp = point_frames(s, sm, P_LOOP_START_S);
        double lep = point_frames(s, sm, P_LOOP_END_S);
        ls = sm->loop_start; le = sm->loop_end;
        if (lsp > 0.0 || lep > 0.0) {
            ls = lsp >= sm->frames ? sm->frames : (uint32_t)lsp;
            le = (lep <= 0.0 || lep >= sm->frames) ? sm->frames : (uint32_t)lep;
        }
        if (le <= ls + 16) {
            le = ls + 16 < sm->frames ? ls + 16 : sm->frames;
            if (le <= ls + 4) { ls = 0; le = sm->frames; }
        }
    }

    for (int vi = 0; vi < MAX_VOICES; ++vi) {
        Voice* v = &s->voices[vi];
        if (v->stage == ENV_OFF) continue;
        if (v->pitch != v->target) { /* glide */
            double d = v->glide_step * (to - from);
            if (v->glide_step <= 0 || fabs(v->target - v->pitch) <= d) v->pitch = v->target;
            else v->pitch += v->target > v->pitch ? d : -d;
        }
        double semis = v->pitch - root + tune + s->bend * bend_r;
        if (vib_depth > 0.f) { /* vibrato: fades in over 200 ms after the delay */
            double t = v->vib_t / s->rate - vib_delay;
            if (t > 0) {
                double fade = t < 0.2 ? t / 0.2 : 1.0;
                semis += vib_depth / 100.0 * fade * sin(2 * M_PI * vib_rate * t);
            }
        }
        v->vib_t += to - from;
        double ratio = pow(2.0, semis / 12.0);
        double inc = base * ratio;
        double ph_inc = 0; int wl = 0;
        if (osc >= 0) {
            double f = 261.6255653 * ratio;
            ph_inc = f / s->rate;
            wl = f > 27.5 ? (int)floor(log2(f / 27.5)) : 0;
            if (wl >= WT_LEVELS) wl = WT_LEVELS - 1;
        }
        float d_step = (1.f - sus) * ms_to_step(d_ms, s->rate);
        float g = level * v->vel_gain;
        float ic1l = v->fic1[0], ic2l = v->fic2[0], ic1r = v->fic1[1], ic2r = v->fic2[1];

        for (uint32_t n = from; n < to; ++n) {
            switch (v->stage) {
            case ENV_ATTACK:
                v->env += a_step;
                if (v->env >= 1.f) { v->env = 1.f; v->stage = ENV_DECAY; }
                break;
            case ENV_DECAY:
                v->env -= d_step;
                if (v->env <= sus || d_step <= 0) { v->env = sus; v->stage = ENV_SUSTAIN; }
                break;
            case ENV_SUSTAIN:
                v->env = sus;
                if (sus <= 0.f && mode != MODE_ONESHOT) v->stage = ENV_OFF;
                break;
            case ENV_RELEASE:
                v->env -= v->rel_step;
                if (v->env <= 0.f) { v->env = 0.f; v->stage = ENV_OFF; }
                break;
            }
            if (v->stage == ENV_OFF) break;

            float xl, xr;
            if (osc >= 0) {
                xl = xr = osc_sample(osc, wl, v->phase);
                v->phase += ph_inc;
                if (v->phase >= 1.0) v->phase -= floor(v->phase);
            } else {
                if (mode == MODE_LOOP && v->pos >= le) v->pos = ls + fmod(v->pos - ls, (double)(le - ls));
                if (v->pos >= sm->frames) { v->stage = ENV_OFF; break; }
                uint32_t i = (uint32_t)v->pos;
                float t = (float)(v->pos - i);
                xl = hermite(sm->l, i, t);
                xr = hermite(sm->r, i, t);
                v->pos += inc;
            }

            if (ftype) {
                /* filter envelope */
                switch (v->fstage) {
                case ENV_ATTACK:  v->fenv += fa_step; if (v->fenv >= 1.f) { v->fenv = 1.f; v->fstage = ENV_DECAY; } break;
                case ENV_DECAY:   v->fenv -= fd_step; if (v->fenv <= fsus || fd_step <= 0) { v->fenv = fsus; v->fstage = ENV_SUSTAIN; } break;
                case ENV_SUSTAIN: v->fenv = fsus; break;
                case ENV_RELEASE: v->fenv -= v->frel_step; if (v->fenv < 0.f) v->fenv = 0.f; break;
                }
                if ((v->fcount++ & 15) == 0) { /* coefficients every 16 samples */
                    float u = s->cut_smooth + (fenv_on ? famt * v->fenv : 0.f);
                    u = u < 0 ? 0 : u > 100 ? 100 : u;
                    float hz = 20.f * powf(1000.f, u / 100.f);
                    if (hz > nyq) hz = nyq;
                    float gg = tanf((float)M_PI * hz / (float)s->rate);
                    v->fa1 = 1.f / (1.f + gg * (gg + fk)); v->fa2 = gg * v->fa1; v->fa3 = gg * v->fa2;
                }
                float a1 = v->fa1, a2 = v->fa2, a3 = v->fa3;
                float v3 = xl - ic2l, v1 = a1 * ic1l + a2 * v3, v2 = ic2l + a2 * ic1l + a3 * v3;
                ic1l = 2.f * v1 - ic1l; ic2l = 2.f * v2 - ic2l;
                float yl = ftype == 1 ? v2 : ftype == 2 ? v1 * fk : xl - fk * v1 - v2;
                v3 = xr - ic2r; v1 = a1 * ic1r + a2 * v3; v2 = ic2r + a2 * ic1r + a3 * v3;
                ic1r = 2.f * v1 - ic1r; ic2r = 2.f * v2 - ic2r;
                float yr = ftype == 1 ? v2 : ftype == 2 ? v1 * fk : xr - fk * v1 - v2;
                xl = yl; xr = yr;
            }
            float e = v->env * g;
            s->v_l[n - s->v_base] += xl * e;
            s->v_r[n - s->v_base] += xr * e;
        }
        v->fic1[0] = ic1l; v->fic2[0] = ic2l; v->fic1[1] = ic1r; v->fic2[1] = ic2r;
    }
}

static void finish_recording(KeySampler* s)
{
    uint32_t tail = (uint32_t)(0.05 * s->rate); /* keep 50 ms after the last loud sample */
    uint32_t len = s->rec_last_loud + tail;
    if (len > s->rec_pos) len = s->rec_pos;
    if (len < 64) { atomic_store(&s->rec_state, REC_IDLE); return; }
    /* short fade-out so the end never clicks */
    uint32_t fade = len < 256 ? len : 256;
    for (uint32_t i = 0; i < fade; ++i) {
        float g = (float)i / fade;
        s->rec_l[len - fade + i] *= 1.f - g;
        s->rec_r[len - fade + i] *= 1.f - g;
    }
    s->rec_len = len;
    s->rec_slot = KEY((int)ctl(s, P_BANK, 1, 1, MAX_BANKS), (int)ctl(s, P_SLOT, 1, 1, MAX_SLOTS));
    atomic_store(&s->rec_state, REC_HANDOFF);
}

/* in_l/in_r here are private copies, so in-place hosts are fine */
static void record(KeySampler* s, const float* l, const float* r, uint32_t n)
{
    int sw = ctl(s, P_RECORD, 0, 0, 1) > 0.5f;
    if ((int)ctl(s, P_SLOT, 1, 1, MAX_SLOTS) >= OSC_FIRST) sw = 0; /* oscillator slots can't be recorded over */
    int st = atomic_load(&s->rec_state);
    if (sw && !s->rec_prev_switch && st == REC_IDLE) { st = REC_ARMED; s->pre_pos = 0;
        memset(s->pre_l, 0, sizeof s->pre_l); memset(s->pre_r, 0, sizeof s->pre_r); }
    if (!sw && st == REC_ARMED) st = REC_IDLE;
    s->rec_prev_switch = sw;
    atomic_store(&s->rec_state, st);

    if (st == REC_RECORDING && !sw) { finish_recording(s); return; }
    if (st != REC_ARMED && st != REC_RECORDING) return;

    float thr = powf(10.f, ctl(s, P_THRESH, -50, -80, -6) / 20.f);
    uint32_t silence_end = (uint32_t)(0.4 * s->rate);

    for (uint32_t i = 0; i < n; ++i) {
        float a = fmaxf(fabsf(l[i]), fabsf(r[i]));
        if (st == REC_ARMED) {
            if (a < thr) {
                s->pre_l[s->pre_pos % PREROLL] = l[i];
                s->pre_r[s->pre_pos % PREROLL] = r[i];
                s->pre_pos++;
                continue;
            }
            /* trigger: start with the pre-roll so the attack isn't cut */
            uint32_t have = s->pre_pos < PREROLL ? s->pre_pos : PREROLL;
            for (uint32_t k = 0; k < have; ++k) {
                uint32_t idx = (s->pre_pos - have + k) % PREROLL;
                s->rec_l[k] = s->pre_l[idx];
                s->rec_r[k] = s->pre_r[idx];
            }
            s->rec_pos = have;
            s->rec_last_loud = have;
            s->rec_silent = 0;
            st = REC_RECORDING;
            atomic_store(&s->rec_state, st);
        }
        /* recording */
        s->rec_l[s->rec_pos] = l[i];
        s->rec_r[s->rec_pos] = r[i];
        s->rec_pos++;
        if (a >= thr) { s->rec_last_loud = s->rec_pos; s->rec_silent = 0; }
        else if (++s->rec_silent >= silence_end) { finish_recording(s); return; }
        if (s->rec_pos >= s->rec_cap) { finish_recording(s); return; }
    }
}

static void run_block(KeySampler* s, uint32_t off, uint32_t nframes, uint32_t* ev_t,
                      const LV2_Atom_Event** ev)
{
    /* copy the input first: the host may give us the same buffer for in and out */
    const float* il = s->in_l ? s->in_l + off : NULL;
    const float* ir = s->in_r ? s->in_r + off : il;
    if (il) memcpy(s->scr_l, il, nframes * sizeof(float)); else memset(s->scr_l, 0, nframes * sizeof(float));
    if (ir) memcpy(s->scr_r, ir, nframes * sizeof(float)); else memcpy(s->scr_r, s->scr_l, nframes * sizeof(float));

    record(s, s->scr_l, s->scr_r, nframes);

    memset(s->v_l, 0, nframes * sizeof(float));
    memset(s->v_r, 0, nframes * sizeof(float));
    s->v_base = off;

    /* MIDI events that fall inside this chunk, sample-accurate */
    uint32_t t = off, end = off + nframes;
    while (*ev && !lv2_atom_sequence_is_end(&s->midi_in->body, s->midi_in->atom.size, *ev)) {
        int64_t f = (*ev)->time.frames;
        uint32_t et = f < 0 ? 0 : (uint32_t)f;
        if (et >= end) break;
        if (et < t) et = t;
        render(s, t, et);
        t = et;
        if ((*ev)->body.type == s->midi_event)
            handle_midi(s, (const uint8_t*)LV2_ATOM_BODY_CONST(&(*ev)->body), (*ev)->body.size);
        *ev = lv2_atom_sequence_next(*ev);
    }
    render(s, t, end);
    (void)ev_t;

    float thru = ctl(s, P_THRU, 1, 0, 1) > 0.5f ? 1.f : 0.f;
    float pan = ctl(s, P_PAN, 0, -1, 1);
    float gl = (pan > 0 ? 1.f - pan : 1.f) * s->expr, gr = (pan < 0 ? 1.f + pan : 1.f) * s->expr;
    for (uint32_t i = 0; i < nframes; ++i) {
        s->out_l[off + i] = s->scr_l[i] * thru + s->v_l[i] * gl;
        s->out_r[off + i] = s->scr_r[i] * thru + s->v_r[i] * gr;
    }
}

static void run(LV2_Handle h, uint32_t nframes)
{
    KeySampler* s = h;
    s->clock_frac += nframes * 1000.0 / s->rate;
    if (s->clock_frac >= 1.0) { unsigned add = (unsigned)s->clock_frac; s->clock_frac -= add; atomic_fetch_add(&s->clock_ms, add); }
    if (!s->out_l || !s->out_r) return;

    int learn = (int)(port_raw(s, P_LEARN) + 0.5f);
    if (learn != s->learn_prev) {
        s->learn_prev = learn;
        s->learn_armed = learn > T_NONE && learn < T_COUNT;
        if (learn == T_RESET) { cc_reset(s); atomic_store(&s->map_dirty, 1); }
    }
    for (int t = 1; t < T_COUNT; ++t)
        if (t_port[t] >= 0 && s->ov_on[t] && target_snapshot(s, t) != s->ov_snap[t]) s->ov_on[t] = 0;

    {   /* slots 61-64: built-in oscillators */
        int slot = (int)ctl(s, P_SLOT, 1, 1, MAX_SLOTS);
        int ow = slot >= OSC_FIRST ? slot - OSC_FIRST : -1;
        if (ow != s->osc_wave) {
            for (int i = 0; i < MAX_VOICES; ++i) s->voices[i].stage = ENV_OFF;
            s->osc_wave = ow;
        }
    }
    atomic_store(&s->want_slot, KEY((int)ctl(s, P_BANK, 1, 1, MAX_BANKS), (int)ctl(s, P_SLOT, 1, 1, MAX_SLOTS)));

    Sample* next = atomic_exchange(&s->pending, NULL);
    if (next) { /* new sound: stop voices of the old one, hand it back for freeing */
        for (int i = 0; i < MAX_VOICES; ++i) s->voices[i].stage = ENV_OFF;
        Sample* old = s->cur;
        s->cur = next;
        if (old) atomic_store(&s->retired, old);
    }

    const LV2_Atom_Event* ev = s->midi_in ? lv2_atom_sequence_begin(&s->midi_in->body) : NULL;
    for (uint32_t off = 0; off < nframes; off += SCRATCH) {
        uint32_t n = nframes - off < SCRATCH ? nframes - off : SCRATCH;
        run_block(s, off, n, NULL, &ev);
    }
}

static void activate(LV2_Handle h)
{
    KeySampler* s = h;
    memset(s->voices, 0, sizeof s->voices);
    s->bend = 0;
    s->pedal = 0;
    s->n_held = 0;
    s->have_last = 0;
    s->cut_smooth = -1.f;
}

static void cleanup(LV2_Handle h)
{
    KeySampler* s = h;
    atomic_store(&s->running, 0);
    if (s->thread_ok) pthread_join(s->thread, NULL);
    free_sample(s->cur);
    free_sample(atomic_load(&s->pending));
    free_sample(atomic_load(&s->retired));
    grpc_close(&s->gc);
    free(s->rec_l);
    free(s->rec_r);
    pthread_mutex_destroy(&s->file_lock);
    free(s);
}

/* ---------------------------------------------------------- LV2 state
 * When the S2400 saves a project, the DSP Card asks every plugin for its state.
 * KeySampler puts the recordings of its current bank into that state, so they
 * are stored inside the project. Loading the project gives them back: they are
 * written to the card's recordings folder and the current slot is reloaded. */

#define STATE_VERSION 1
#define STATE_MAX_BYTES (24u << 20) /* skip absurdly large files */

static LV2_State_Status state_save(LV2_Handle h, LV2_State_Store_Function store,
                                   LV2_State_Handle sh, uint32_t flags,
                                   const LV2_Feature* const* features)
{
    (void)flags; (void)features;
    KeySampler* s = h;
    int32_t version = STATE_VERSION;
    int32_t bank = (int32_t)ctl(s, P_BANK, 1, 1, MAX_BANKS);
    store(sh, s->k_version, &version, sizeof version, s->u_int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    store(sh, s->k_bank, &bank, sizeof bank, s->u_int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

    pthread_mutex_lock(&s->file_lock);
    for (int slot = 1; slot <= MAX_SLOTS; ++slot) {
        char path[PATH_LEN];
        slot_path(s, KEY(bank, slot), path);
        size_t len;
        void* data = read_file(path, &len);
        if (data && len <= STATE_MAX_BYTES)
            store(sh, s->k_slot[slot], data, len, s->u_chunk, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        free(data);
    }
    pthread_mutex_unlock(&s->file_lock);
    return LV2_STATE_SUCCESS;
}

static LV2_State_Status state_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                      LV2_State_Handle sh, uint32_t flags,
                                      const LV2_Feature* const* features)
{
    (void)flags; (void)features;
    KeySampler* s = h;
    size_t size; uint32_t type, fl;
    const void* v = retrieve(sh, s->k_version, &size, &type, &fl);
    if (!v || type != s->u_int) return LV2_STATE_SUCCESS; /* not a KeySampler state */
    v = retrieve(sh, s->k_bank, &size, &type, &fl);
    int bank = (v && type == s->u_int) ? *(const int32_t*)v : 1;
    if (bank < 1 || bank > MAX_BANKS) bank = 1;

    char dir[PATH_LEN];
    pthread_mutex_lock(&s->file_lock);
    mkdir(s->rec_dir, 0755);
    bank_dir(s, bank, dir);
    mkdir(dir, 0755);
    int restored = 0;
    for (int slot = 1; slot <= MAX_SLOTS; ++slot) {
        char path[PATH_LEN];
        slot_path(s, KEY(bank, slot), path);
        const void* data = retrieve(sh, s->k_slot[slot], &size, &type, &fl);
        if (data && type == s->u_chunk && size > 44) {
            /* skip the write if the card already has exactly this file */
            size_t have_len;
            void* have = read_file(path, &have_len);
            int same = have && have_len == size && !memcmp(have, data, size);
            free(have);
            if (!same) write_file_atomic(path, data, size);
            ++restored;
        } else {
            unlink(path); /* the project had nothing in this slot */
        }
    }
    pthread_mutex_unlock(&s->file_lock);
    fprintf(stderr, "KeySampler: restored %d recording(s) into bank %d from saved state\n", restored, bank);
    atomic_store(&s->force_reload, 1);
    return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri)
{
    static const LV2_State_Interface state = { state_save, state_restore };
    return !strcmp(uri, LV2_STATE__interface) ? &state : NULL;
}

static const LV2_Descriptor descriptor = {
    KEYSAMPLER_URI, instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
