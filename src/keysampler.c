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
#include "port_defaults.h"

#define KEYSAMPLER_URI "https://github.com/keysampler/KeySampler"
#define MAX_VOICES 16
#define MAX_SAMPLE 10002    /* Sample 0-3 = oscillators (shown by name), 4-10002 = recordings
                             * shown as 1-9999 and stored as sample_0001.wav ... sample_9999.wav */
#define OSC_COUNT 4
/* Sound menu: 0-3 oscillators, 4 "Sample" (this instance's own recording, number in the
 * hidden Sample ID), 5-10003 recordings 1-9999 picked by number (to play or delete). */
#define SOUND_SAMPLE 4
#define SOUND_MAX 10003
#define SAMPLE_ID_MAX 9999
#define CMD_DWELL_MS 900  /* Save/Restore item: how long a choice must stay selected */
#define MAX_SEC 59         /* start/loop points: seconds + ms + samples, */
#define MAX_SMP 95         /* each a small whole number, like the S2400's s.mmm+samples */
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
    P_SAMPLE,
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
    P_VIB_RATE,
    P_PAN,
    P_VOICES,
    P_MENU_CMD,
    /* ---- beyond the S2400's 32: defaults, CC-controllable ---- */
    P_THRU,
    P_VELSENS,
    P_BEND,
    P_VIB_DELAY,
    P_START_SMP,
    P_LOOP_START_SMP,
    P_LOOP_END_SMP,
    P_FENV_ON,
    P_MENU_SYNC,
    P_LFO_DEST,
    P_SAMPLE_ID,      /* hidden: number of the recording "Sample" plays (0 = none) */
    P_COUNT
};
#define P_FIRST_CTL P_SAMPLE

/* ms and samples ports belonging to a point's "sec" port */
static int point_ms(int p_s) { return p_s == P_START_S ? P_START_MS : p_s == P_LOOP_START_S ? P_LOOP_START_MS : P_LOOP_END_MS; }
static int point_smp(int p_s) { return p_s == P_START_S ? P_START_SMP : p_s == P_LOOP_START_S ? P_LOOP_START_SMP : P_LOOP_END_SMP; }

/* Things a MIDI CC can control (also the "CC Learn" menu, in this order). */
enum { T_NONE = 0, T_LEVEL, T_CUTOFF, T_RESO, T_ATTACK, T_DECAY, T_SUSTAIN, T_RELEASE, T_TUNE,
       T_START, T_LOOP_START, T_LOOP_END, T_SAMPLE, T_RECORD, T_FILTER, T_MODE,
       T_PAN, T_VIB_RATE, T_VIB_DEPTH, T_VIB_DELAY, T_THRU, T_GLIDE_MODE, T_GLIDE,
       T_FENV_ON, T_FENV_AMT, T_FENV_A, T_FENV_D, T_FENV_S, T_FENV_R,
       T_SAVE, T_RESTORE, /* actions, not parameters */
       T_LFO_DEST,
       T_SAMPLE_ID,
       T_COUNT,
       T_RESET = T_COUNT, T_SAVE_NOW, T_RESTORE_NOW }; /* CC Learn menu commands */
static const int t_port[T_COUNT] = { -1, P_LEVEL, P_CUTOFF, P_RESO, P_ATTACK, P_DECAY, P_SUSTAIN,
    P_RELEASE, P_TUNE, P_START_S, P_LOOP_START_S, P_LOOP_END_S, P_SAMPLE, P_RECORD, P_FILTER, P_MODE,
    P_PAN, P_VIB_RATE, P_VIB_DEPTH, P_VIB_DELAY, P_THRU, P_GLIDE_MODE, P_GLIDE,
    P_FENV_ON, P_FENV_AMT, P_FENV_A, P_FENV_D, P_FENV_S, P_FENV_R, -1, -1, P_LFO_DEST, P_SAMPLE_ID };

#define CC_PRESSURE 128    /* channel aftertouch, handled like a 129th CC */
#define CC_SLOTS 129
/* Defaults = the Edirol PCR-M30/50/80 "GM2 SET" (memory 0), owner's manual p. 98.
 * Knobs follow the printed template; the sliders carry ADSR and the points.
 * Fixed (not in this table): R6/R7 RPN coarse/fine tune, P1 hold, P2 expression,
 * Buttons: B1 (program -) = filter type, B2 (program +) = play mode, B3 (CC65) = glide on/off,
 * B4 (CC127) = LFO destination pitch <-> filter, B5 unused, B6 (GM2 On SysEx) = sliders S1-S4 volume <-> filter envelope.
 * L1-L3 (re-assigned on the PCR to CC 80/81/82, latch mode) = record, save, restore. */
static const struct { int cc, t; } cc_defaults[] = {
    { 74, T_CUTOFF },     /* R1 FILTER CUTOFF */
    { 71, T_RESO },       /* R2 FILTER RESONANCE */
    { 76, T_VIB_RATE },   /* R3 VIBRATO RATE */
    { 77, T_VIB_DEPTH },  /* R4 VIBRATO DEPTH */
    { 78, T_VIB_DELAY },  /* R5 VIBRATO DELAY */
    { 10, T_PAN },        /* R8 PAN */
    /* S1-S4: Attack, Decay, Sustain, Release of the volume envelope, or of the
     * filter envelope after B6 (see handle_cc) */
    { 5, T_ATTACK },      /* S1 (portamento time) */
    { CC_PRESSURE, T_DECAY }, /* S2 (aftertouch) */
    { 73, T_SUSTAIN },    /* S3 (envelope attack) */
    { 75, T_RELEASE },    /* S4 (envelope decay) */
    { 72, T_START },      /* S5 (envelope release) -> sample start */
    { 93, T_LOOP_START }, /* S6 (chorus) */
    { 91, T_LOOP_END },   /* S7 (reverb) */
    { 7, T_LEVEL },       /* S8 VOLUME */
};

/* ---------------------------------------------------------- oscillators
 * Slots 61-64 play built-in waveforms. Each has one table per octave with only
 * the harmonics that stay below ~20 kHz in that octave (band-limited), so high
 * notes don't alias. Root Note plays middle C (261.63 Hz). */
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

enum { MODE_ONESHOT = 0, MODE_GATE = 1, MODE_LOOP = 2, MODE_PINGPONG = 3 };
enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { REC_IDLE = 0, REC_ARMED, REC_RECORDING, REC_HANDOFF };

typedef struct {
    float*   l; /* de-interleaved; frame k stored at [k+1]; padded for interpolation */
    float*   r;
    uint32_t frames;
    double   rate;
    uint32_t loop_start, loop_end;
    int      mono;  /* left == right: the filter runs once per voice */
} Sample;

typedef struct { int init, prev, done, armed, armed_num; unsigned since, armed_ms; } MenuCmd;

typedef struct {
    int    stage;
    int    note;
    int    held;
    int    sustained;
    double pos;
    int    rev;     /* ping-pong: playing backwards */
    float  env;
    float  vel_gain;
    float  rel_step;
    uint32_t age;
    uint32_t vib_t; /* frames since note-on, for the LFO delay */
    float    lfo_cut; /* LFO -> cutoff offset (0..100 units) */
    float    lfo_amp; /* LFO -> volume factor (0..1) */
    float    lfo_prev_amp; /* last block's factor, ramped from to avoid zipper noise */
    double   pitch, target, glide_step; /* current / target note (semitones), glide speed per frame */
    double   phase;                     /* oscillator slots */
    /* per-voice filter + its envelope */
    float    lad[2][4];   /* ladder filter: 4 stage states per channel */
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
    MenuCmd  cmds[2];              /* Settings item, Record item */
    atomic_int map_dirty;

    /* Menu sync: after a controller move, the loader thread asks sushi (the
     * DSP Card's audio engine) to set the same parameter, like the S2400 menu
     * does, so the new value can show up in the menu and be saved. */
    float    sync_val[P_COUNT];        /* domain values to send (audio thread writes) */
    _Atomic unsigned long long sync_dirty; /* one bit per port */
    atomic_uint clock_ms, sync_last_ms, sync_first_ms;
    atomic_int sync_now;               /* explicit Save: send without waiting */
    unsigned action_ms;                /* debounce for Save/Restore controls */
    int      fenv_page;                /* 1 = sliders S1-S4 control the filter envelope */
    double   clock_frac;
    GrpcConn gc;
    int      sync_proc;                /* sushi processor id of this instance, -1 = unknown */
    int      sync_pid[P_COUNT];
    float    sync_min[P_COUNT], sync_max[P_COUNT];
    uint32_t sync_retry_ms;
    LV2_URID_Map* map;
    LV2_URID u_chunk, u_int, k_version, k_number, k_data;
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
    atomic_int rec_done;       /* number of a finished recording, for the audio thread */
    int        own_slot;       /* slot "Sample" plays (kept here too, in case Sample ID is not echoed back) */
    int        prev_own_slot;  /* the one before, for when the current one gets deleted */
    atomic_int want_is_num;    /* Sound is a number (loader: adopt it as "Sample" once it loads) */
    atomic_int adopt;          /* loader -> audio thread: this number loaded, make it "Sample" */
    atomic_int del_req;        /* recording number to delete (loader thread) */
    unsigned   del_armed_ms;   /* Delete must be chosen twice within 5 s */
    int        del_armed_num;
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

static void mark_mono(Sample* smp)
{
    if (!smp) return;
    smp->mono = smp->l == smp->r || !memcmp(smp->l, smp->r, (smp->frames + 4) * sizeof(float));
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

/* Sample n (4..9999): <recordings>/sample_NNNN.wav, else samples/sample_NNNN.wav in the
 * plugin folder (WAVs added with Sync Files). 0-3 are the oscillators: no file. */
/* Sample value n (4..10002) is recording number n - 3 (1..9999), as the menu shows it */
static void rec_path(const KeySampler* s, int n, char* out)
{
    snprintf(out, PATH_LEN, "%.900ssample_%04d.wav", s->rec_dir, n - (OSC_COUNT - 1));
}

static int path_for_sample(const KeySampler* s, int n, char* out)
{
    struct stat st;
    if (n < OSC_COUNT) return 0;
    rec_path(s, n, out);
    if (stat(out, &st) == 0) return 1;
    snprintf(out, PATH_LEN, "%.900ssamples/sample_%04d.wav", s->bundle, n - (OSC_COUNT - 1));
    return stat(out, &st) == 0;
}

/* A number no file uses yet: one more than the highest, or the first gap once 9999 is taken. */
/* The highest number ever given out is kept in last_number.txt, so a deleted
 * recording's number is not handed to the next take (an old project may still
 * point at it). Only after 9999 are freed numbers reused, lowest first. */
static int read_last_number(const KeySampler* s)
{
    char path[PATH_LEN];
    snprintf(path, PATH_LEN, "%.900slast_number.txt", s->rec_dir);
    FILE* f = fopen(path, "r");
    int n = 0;
    if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
    return n < 0 ? 0 : n > MAX_SAMPLE ? MAX_SAMPLE : n;
}

static void write_last_number(const KeySampler* s, int n)
{
    char path[PATH_LEN], tmp[PATH_LEN];
    snprintf(path, PATH_LEN, "%.900slast_number.txt", s->rec_dir);
    snprintf(tmp, PATH_LEN, "%.900slast_number.tmp", s->rec_dir);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d\n", n);
    fclose(f);
    rename(tmp, path);
}

static int next_free_sample(const KeySampler* s)
{
    char path[PATH_LEN];
    int hi = read_last_number(s);
    if (hi < OSC_COUNT - 1) hi = OSC_COUNT - 1;
    for (int n = MAX_SAMPLE; n > hi; --n)                 /* files newer than the record */
        if (path_for_sample(s, n, path)) { hi = n; break; }
    for (int n = hi + 1; n <= MAX_SAMPLE; ++n)
        if (!path_for_sample(s, n, path)) return n;
    for (int n = OSC_COUNT; n <= MAX_SAMPLE; ++n)         /* 9999 reached: reuse freed numbers */
        if (!path_for_sample(s, n, path)) return n;
    return 0;
}

static void migrate_one(const char* from, const char* to)
{
    struct stat st;
    if (stat(from, &st) != 0 || stat(to, &st) == 0) return;
    if (rename(from, to) == 0) fprintf(stderr, "KeySampler: %s -> %s\n", from, to);
}

/* Earlier versions: 0.19 named recordings sample_NNN.wav (same numbers, now 4 digits);
 * up to 0.18 they were bank/slot, now sample bank*100+slot. */
static void migrate_old_names(const KeySampler* s)
{
    char from[PATH_LEN], to[PATH_LEN];
    {   /* 0.20-0.30 named recordings after the Sample value (first = sample_0004.wav);
         * since 0.31 after the number the menu shows (first = sample_0001.wav). Once. */
        char mark[PATH_LEN]; struct stat st;
        snprintf(mark, PATH_LEN, "%.900snumbered_from_1", s->rec_dir);
        if (stat(mark, &st) != 0) {
            for (int n = OSC_COUNT; n <= 9999; ++n) {
                snprintf(from, PATH_LEN, "%.900ssample_%04d.wav", s->rec_dir, n);
                snprintf(to, PATH_LEN, "%.900ssample_%04d.wav", s->rec_dir, n - (OSC_COUNT - 1));
                migrate_one(from, to);
            }
            mkdir(s->rec_dir, 0755);
            FILE* f = fopen(mark, "w");
            if (f) { fputs("recordings are named after the number shown in the menu\n", f); fclose(f); }
        }
    }
    for (int n = OSC_COUNT; n <= 999; ++n) {
        snprintf(from, PATH_LEN, "%.900ssample_%03d.wav", s->rec_dir, n);
        rec_path(s, n, to);
        migrate_one(from, to);
    }
    for (int bank = 1; bank <= 99; ++bank)
        for (int slot = 1; slot <= 60; ++slot) {
            if (bank == 1) snprintf(from, PATH_LEN, "%.900sslot%02d.wav", s->rec_dir, slot);
            else snprintf(from, PATH_LEN, "%.900sbank%02d/slot%02d.wav", s->rec_dir, bank, slot);
            rec_path(s, bank * 100 + slot, to);
            migrate_one(from, to);
        }
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
    if (!path_for_sample(s, atomic_load(&s->want_slot), path) || stat(path, &st) != 0) {
        *empty = s->loaded_path[0] != 0;
        s->loaded_path[0] = 0;
        return NULL;
    }
    if (!strcmp(path, s->loaded_path) && st.st_mtime == s->loaded_mtime && st.st_size == s->loaded_size)
        return NULL;
    remember_loaded(s, path);
    Sample* smp = load_wav(path);
    mark_mono(smp);
    return smp;
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
    mkdir(s->rec_dir, 0755);
    pthread_mutex_lock(&s->file_lock);
    int num = next_free_sample(s), saved = -1;
    if (num > 0) { rec_path(s, num, path); saved = save_wav(path, smp); }
    if (saved == 0 && num > read_last_number(s)) write_last_number(s, num);
    pthread_mutex_unlock(&s->file_lock);
    if (saved != 0) {
        fprintf(stderr, "KeySampler: recorded %u frames, but could not save it (%s)\n", n,
                num > 0 ? strerror(errno) : "all numbers used");
        free_sample(smp);
        return;
    }
    fprintf(stderr, "KeySampler: recorded %u frames into %s\n", n, path);
    remember_loaded(s, path);
    atomic_store(&s->want_slot, num);     /* so the loader doesn't reload the old one meanwhile */
    mark_mono(smp);
    atomic_store(&s->pending, smp);
    atomic_store(&s->rec_done, num);      /* audio thread: select it and write it to the menu */
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

__attribute__((unused)) static void cc_map_load(KeySampler* s)
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
    "sample", "record", "threshold", "mode", "level", "root", "tune",
    "attack", "decay", "sustain", "release",
    "start_sec", "start_msec", "loop_start_sec", "loop_start_msec", "loop_end_sec", "loop_end_msec",
    "filter", "cutoff", "resonance",
    "fenv_amount", "fenv_attack", "fenv_decay", "fenv_sustain", "fenv_release",
    "glide_mode", "glide", "vib_depth", "vib_rate", "ks_pan", "voices", "save_restore",
    "thru", "velsens", "bend", "vib_delay",
    "start_smp", "loop_start_smp", "loop_end_smp", "fenv", "menu_sync", "lfo_dest", "sample_id",
};

static float port_raw(const KeySampler* s, int p);
static int ictl(const KeySampler* s, int p, float def, float lo, float hi);

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
            if (pi == P_MENU_CMD || pi == P_RECORD) continue;
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
            int del = atomic_exchange(&s->del_req, 0);
            if (del >= OSC_COUNT) {  /* only recordings; WAVs from Sync Files stay */
                char path[PATH_LEN];
                rec_path(s, del, path);
                pthread_mutex_lock(&s->file_lock);
                int r = unlink(path);
                pthread_mutex_unlock(&s->file_lock);
                fprintf(stderr, "KeySampler: delete %s: %s\n", path, r == 0 ? "done" : strerror(errno));
                int empty;                       /* go silent now if it was playing */
                Sample* n = check_selection(s, &empty);
                if (!n && empty) n = silent_sample();
                if (n) atomic_store(&s->pending, n);
            }
            int forced = atomic_exchange(&s->force_reload, 0);
            if (forced) s->loaded_path[0] = 0;
            if (forced || slot != last_slot || tick % 40 == 0) { /* slot change: now; rescan: every 2 s */
                last_slot = slot;
                int empty;
                Sample* n = check_selection(s, &empty);
                if (!n && empty) n = silent_sample();
                if (n) atomic_store(&s->pending, n);
                if (n && n->frames && slot >= OSC_COUNT && atomic_load(&s->want_is_num)) atomic_store(&s->adopt, slot);
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
    s->k_number = map->map(map->handle, KEYSAMPLER_URI "#sampleNumber");
    s->k_data = map->map(map->handle, KEYSAMPLER_URI "#sampleData");
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
    atomic_init(&s->want_slot, 0);
    atomic_init(&s->rec_done, 0);
    s->own_slot = s->prev_own_slot = -1;
    atomic_init(&s->want_is_num, 0);
    atomic_init(&s->adopt, 0);
    atomic_init(&s->del_req, 0);
    atomic_init(&s->running, 1);
    atomic_init(&s->rec_state, REC_IDLE);

    migrate_old_names(s);
    int empty;
    s->cur = check_selection(s, &empty); /* first sound ready before audio starts */
    for (int t = 1; t < T_COUNT; ++t) if (t_port[t] >= 0) s->port_target[t_port[t]] = (int8_t)t;
    cc_reset(s); /* controller map: fixed PCR-M30 GM2 defaults (no CC Learn since 0.21) */
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
    case T_SAMPLE:  return floorf(v * SOUND_MAX + 0.5f);
    case T_SAMPLE_ID: return floorf(v * SAMPLE_ID_MAX + 0.5f);
    case T_RECORD:  return v >= 0.5f ? 1.f : 0.f;
    case T_FILTER:  return floorf(v * 3.999f);
    case T_MODE:    return floorf(v * 3.999f);
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
    case T_LFO_DEST: return floorf(v * 2.999f);
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

/* whole-number parameters: round, never truncate. Values travel to and from the S2400
 * normalized (0..1), so e.g. Sample 5 can come back as 4.9999995. */
static int ictl(const KeySampler* s, int p, float def, float lo, float hi)
{
    return (int)lrintf(ctl(s, p, def, lo, hi));
}

/* What the Sound menu selects, as a slot: 0-3 oscillators, 4-10002 recording (slot - 3),
 * -1 = "Sample" with no recording yet (silent). */
static int sound_slot(const KeySampler* s)
{
    int v = ictl(s, P_SAMPLE, 0, 0, SOUND_MAX);
    if (v < SOUND_SAMPLE) return v;
    if (v == SOUND_SAMPLE) {
        int id = ictl(s, P_SAMPLE_ID, 0, 0, SAMPLE_ID_MAX);
        return id > 0 ? id + (OSC_COUNT - 1) : s->own_slot;
    }
    return v - 1;
}

static int sound_slot_of_sample(const KeySampler* s)
{
    int id = ictl(s, P_SAMPLE_ID, 0, 0, SAMPLE_ID_MAX);
    return id > 0 ? id + (OSC_COUNT - 1) : s->own_slot;
}

static void set_override(KeySampler* s, int t, float v);
static void queue_sync(KeySampler* s, int t);
/* audio thread: make slot (or -1 = none) what "Sample" plays, and write Sample ID to the menu */
static void set_sample_slot(KeySampler* s, int slot)
{
    int id = slot >= OSC_COUNT ? slot - (OSC_COUNT - 1) : 0;
    s->own_slot = slot >= OSC_COUNT ? slot : -1;
    set_override(s, T_SAMPLE_ID, (float)id / SAMPLE_ID_MAX);
    queue_sync(s, T_SAMPLE_ID);
    atomic_store(&s->sync_now, 1);
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

static void restore_menu(KeySampler* s);
/* Settings > Default: every sound setting back to its default, in the menu too.
 * Sound, Sample ID, Record and Changes are left alone. */
static void defaults_to_menu(KeySampler* s)
{
    restore_menu(s);
    unsigned long long bits = 0;
    for (int p = P_FIRST_CTL; p < P_COUNT; ++p) {
        if (p == P_SAMPLE || p == P_SAMPLE_ID || p == P_RECORD || p == P_MENU_CMD) continue;
        s->sync_val[p] = port_default[p];
        bits |= 1ULL << p;
    }
    atomic_fetch_or(&s->sync_dirty, bits);
    atomic_store(&s->sync_now, 1);
}

/* B6: forget controller changes, play the menu values again */
static void restore_menu(KeySampler* s)
{
    for (int t = 1; t < T_COUNT; ++t)
        if (t != T_SAMPLE && t != T_SAMPLE_ID) s->ov_on[t] = 0;   /* the sound stays */
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
    if (s->fenv_page) /* B6 switched S1-S4 to the filter envelope */
        t = t == T_ATTACK ? T_FENV_A : t == T_DECAY ? T_FENV_D : t == T_SUSTAIN ? T_FENV_S : t == T_RELEASE ? T_FENV_R : t;
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
    int m = ((int)(ctl(s, P_MODE, 1, 0, 3) + 0.5f) + 1) % 4;   /* One Shot > Gate > Loop > Ping-Pong */
    set_override(s, T_MODE, (m + 0.5f) / 4.f);
}
static void toggle_record(KeySampler* s)
{
    set_override(s, T_RECORD, ictl(s, P_RECORD, 0, 0, 1) == 1 ? 0.f : 1.f);
}
/* B4: the LFO (R3-R5) moves the pitch (vibrato), the filter cutoff or the volume (tremolo) */
static void toggle_lfo_dest(KeySampler* s)
{
    int d = ((int)(ctl(s, P_LFO_DEST, 0, 0, 2) + 0.5f) + 1) % 3;
    set_override(s, T_LFO_DEST, (d + 0.5f) / 3.f);
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
    int maxv = gm == GLIDE_LEGATO ? 1 : ictl(s, P_VOICES, MAX_VOICES, 1, MAX_VOICES);
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
    v->rev = 0;
    v->lfo_amp = v->lfo_prev_amp = 1.f;
    v->phase = 0;
    memset(v->lad, 0, sizeof v->lad);
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
    int mode = ictl(s, P_MODE, MODE_GATE, 0, 3);
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
        } else if (m[1] == 65) { toggle_glide(s);                     /* B3 "portamento on/off" */
        } else if (m[1] == 127) { toggle_lfo_dest(s);                 /* B4 "poly mode on": LFO -> pitch / filter */
        } else if (m[1] == 126) {                                      /* B5: not used */
        } else if (m[1] >= 80 && m[1] <= 82) {                         /* L1-L3, set on the PCR to CC 80-82 */
            if (m[2] >= 64) {                                          /* press (latch mode: 127 down, 0 up) */
                unsigned now = atomic_load(&s->clock_ms);
                if (now - s->action_ms >= 200) {
                    s->action_ms = now;
                    if (m[1] == 80) toggle_record(s); else if (m[1] == 81) save_to_menu(s); else restore_menu(s);
                }
            }
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
        if (dir < 0) cycle_filter(s);    /* B1 */
        else cycle_mode(s);              /* B2 */
    } else if (m[0] == 0xF0 && size >= 6 && m[1] == 0x7E && m[3] == 0x09 && m[4] == 0x03) {
        s->fenv_page = !s->fenv_page;  /* B6 "GM2 system on": S1-S4 volume <-> filter envelope */
    }
}

/* Moog-style 4-pole transistor ladder (24 dB/oct), zero-delay feedback with a tanh
 * saturator where the feedback meets the input (after Zavalishin, "The Art of VA
 * Filter Design"). LP = 4th stage; BP and HP are mixed from the stages, as in the
 * Oberheim Xpander. */
#define LADDER_HEADROOM 1.2f  /* saturation knee: loud input and high resonance distort */
static inline float sat(float x)
{
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2); /* fast tanh */
}
static inline float ladder(float x, float* st, float G, float inv, float k, int type)
{
    float b = 1.f - G;
    float S = b * (((G * st[0] + st[1]) * G + st[2]) * G + st[3]);
    float u = LADDER_HEADROOM * sat((x - k * S) * inv * (1.f / LADDER_HEADROOM));
    float y1 = G * u + b * st[0]; st[0] = 2.f * y1 - st[0];
    float y2 = G * y1 + b * st[1]; st[1] = 2.f * y2 - st[1];
    float y3 = G * y2 + b * st[2]; st[2] = 2.f * y3 - st[2];
    float y4 = G * y3 + b * st[3]; st[3] = 2.f * y4 - st[3];
    if (type == 1) return y4;                                     /* low pass, 24 dB */
    if (type == 2) return 4.f * (y2 - 2.f * y3 + y4);             /* band pass, 12+12 dB */
    return u - 4.f * y1 + 6.f * y2 - 4.f * y3 + y4;               /* high pass, 24 dB */
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
    int mode = ictl(s, P_MODE, MODE_GATE, 0, 3);
    if (osc >= 0) mode = MODE_GATE; /* oscillators always sustain while held */
    float vib_rate = ctl(s, P_VIB_RATE, 5, 0.1f, 12);
    float vib_depth = ctl(s, P_VIB_DEPTH, 0, 0, 100);
    double vib_delay = ctl(s, P_VIB_DELAY, 0, 0, 2000) / 1000.0;
    int lfo_dest = (int)(ctl(s, P_LFO_DEST, 0, 0, 2) + 0.5f); /* 0 pitch, 1 filter, 2 volume */

    /* filter: per voice, cutoff = smoothed Cutoff + envelope x Amount */
    int ftype = (int)(ctl(s, P_FILTER, 0, 0, 3) + 0.5f);
    float cut_target = ctl(s, P_CUTOFF, 100, 0, 100);
    if (s->cut_smooth < 0) s->cut_smooth = cut_target;
    s->cut_smooth += (cut_target - s->cut_smooth) * (float)(1.0 - exp(-(double)(to - from) / (0.01 * s->rate)));
    float reso = ctl(s, P_RESO, 0, 0, 1);
    float fk = 4.6f * reso;                         /* ladder feedback: self-oscillates near the top */
    float fdrive = ftype == 3 ? 1.f : 1.f + 0.5f * fk; /* LP/BP: give back part of the bass lost to feedback */
    int fenv_on = ctl(s, P_FENV_ON, 1, 0, 1) > 0.5f;
    float famt = ctl(s, P_FENV_AMT, 0, -100, 100); /* 0 = no filter envelope */
    float fa_step = ms_to_step(ctl(s, P_FENV_A, 5, 0, 10000), s->rate);
    float fsus = ctl(s, P_FENV_S, 0.2f, 0, 1);
    float fd_step = (1.f - fsus) * ms_to_step(ctl(s, P_FENV_D, 300, 0, 10000), s->rate);
    float nyq = (float)(0.45 * s->rate);
    int mono_src = osc >= 0 || (sm && sm->mono);

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
        v->lfo_cut = 0.f; v->lfo_amp = 1.f;
        if (vib_depth > 0.f) { /* LFO: fades in over 200 ms after the delay */
            double t = v->vib_t / s->rate - vib_delay;
            if (t > 0) {
                double fade = t < 0.2 ? t / 0.2 : 1.0;
                double w = fade * sin(2 * M_PI * vib_rate * t);
                if (lfo_dest == 1) v->lfo_cut = (float)(vib_depth * 0.3 * w);  /* depth 100 = +-30 units = +-3 octaves */
                else if (lfo_dest == 2) v->lfo_amp = (float)(1.0 - vib_depth / 100.0 * 0.5 * (1.0 - w)); /* 100 = full tremolo */
                else semis += vib_depth / 100.0 * w;                        /* depth 100 = +-100 cents */
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
        float amp0 = v->lfo_prev_amp, amp_step = (v->lfo_amp - amp0) / (float)(to - from > 0 ? to - from : 1);

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
                if (mode == MODE_PINGPONG) { /* bounce between the loop points */
                    double hi = (double)le - 1.0, lo = (double)ls;
                    for (int k = 0; k < 8 && hi > lo; ++k) {
                        if (!v->rev && v->pos > hi) { v->pos = hi - (v->pos - hi); v->rev = 1; }
                        else if (v->rev && v->pos < lo && v->pos >= 0) { v->pos = lo + (lo - v->pos); v->rev = 0; }
                        else if (v->rev && v->pos < 0) { v->pos = lo; v->rev = 0; }
                        else break;
                    }
                    if (v->pos > hi) v->pos = hi;
                    if (v->pos < 0) v->pos = 0;
                }
                if (v->pos >= sm->frames) { v->stage = ENV_OFF; break; }
                uint32_t i = (uint32_t)v->pos;
                float t = (float)(v->pos - i);
                xl = hermite(sm->l, i, t);
                xr = hermite(sm->r, i, t);
                v->pos += v->rev ? -inc : inc;
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
                    float u = s->cut_smooth + (fenv_on ? famt * v->fenv : 0.f) + v->lfo_cut;
                    u = u < 0 ? 0 : u > 100 ? 100 : u;
                    float hz = 20.f * powf(1000.f, u / 100.f);
                    if (hz > nyq) hz = nyq;
                    float gg = tanf((float)M_PI * hz / (float)s->rate);
                    v->fa1 = gg / (1.f + gg);                 /* one-pole gain G */
                    float G = v->fa1, G4 = G * G * G * G;
                    v->fa2 = 1.f / (1.f + fk * G4);           /* zero-delay feedback solve */
                }
                xl = ladder(xl * fdrive, v->lad[0], v->fa1, v->fa2, fk, ftype);
                xr = mono_src ? xl : ladder(xr * fdrive, v->lad[1], v->fa1, v->fa2, fk, ftype);
            }
            float e = v->env * g * amp0; amp0 += amp_step;
            s->v_l[n - s->v_base] += xl * e;
            s->v_r[n - s->v_base] += xr * e;
        }
        v->lfo_prev_amp = v->lfo_amp;
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
    atomic_store(&s->rec_state, REC_HANDOFF);
}

/* in_l/in_r here are private copies, so in-place hosts are fine */
static void record(KeySampler* s, const float* l, const float* r, uint32_t n)
{
    int sw = ictl(s, P_RECORD, 0, 0, 1) == 1;
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

    /* Changes menu (the S2400 draws its own menus, so no pop-ups):
     *   1 Save, 2 Restore, 3 Default - the sound itself is never touched;
     *   4 Delete - deletes the recording picked by number in Sound (not "Sample", not a
     *   waveform), then Sound goes to "Sample". Afterwards the item goes back to "---".
     * A value counts only after it has stayed selected for CMD_DWELL_MS. Values found
     * when the plugin starts are never acted on. */
    {
        unsigned now = atomic_load(&s->clock_ms);
        MenuCmd* c = &s->cmds[0];
        int v = ictl(s, P_MENU_CMD, 0, 0, 4);
        if (!c->init) { c->init = 1; c->prev = c->done = v; }
        if (v != c->prev) { c->prev = v; c->since = now; }
        else if (v != c->done && now - c->since >= CMD_DWELL_MS) {
            c->done = v;
            if (v == 1) save_to_menu(s);
            else if (v == 2) restore_menu(s);
            else if (v == 3) defaults_to_menu(s);
            else if (v == 4) {
                int sv = ictl(s, P_SAMPLE, 0, 0, SOUND_MAX);
                if (sv > SOUND_SAMPLE) {
                    atomic_store(&s->del_req, sv - 1);
                    int cur = sound_slot_of_sample(s);
                    if (cur == sv - 1) {   /* it was also "Sample": fall back to the one before */
                        int back = s->prev_own_slot != sv - 1 ? s->prev_own_slot : -1;
                        s->prev_own_slot = -1;
                        set_sample_slot(s, back);
                    }
                    set_override(s, T_SAMPLE, (float)SOUND_SAMPLE / SOUND_MAX);
                    queue_sync(s, T_SAMPLE);
                }
            }
            if (v >= 1) {   /* put the item back to "---" in the menu */
                s->sync_val[P_MENU_CMD] = 0;
                atomic_fetch_or(&s->sync_dirty, 1ULL << P_MENU_CMD);
                atomic_store(&s->sync_now, 1);
            }
        }
    }
    for (int t = 1; t < T_COUNT; ++t)
        if (t_port[t] >= 0 && s->ov_on[t] && target_snapshot(s, t) != s->ov_snap[t]) s->ov_on[t] = 0;

    {   /* a recording just finished: play it, and write its number into the menu */
        int done = atomic_exchange(&s->rec_done, 0);
        if (done > 0) {
            int cur = sound_slot_of_sample(s);
            if (cur != done) s->prev_own_slot = cur;
            set_sample_slot(s, done);
            set_override(s, T_SAMPLE, (float)SOUND_SAMPLE / SOUND_MAX);   /* Sound = "Sample" */
            queue_sync(s, T_SAMPLE);
            atomic_store(&s->sync_now, 1);
        }
    }
    {   /* Sample 0-3: built-in oscillators */
        int n = sound_slot(s);
        int ow = n >= 0 && n < OSC_COUNT ? n : -1;
        if (ow != s->osc_wave) {
            for (int i = 0; i < MAX_VOICES; ++i) s->voices[i].stage = ENV_OFF;
            s->osc_wave = ow;
        }
    }
    {   /* a number picked in Sound loaded: it becomes what "Sample" plays */
        int a = atomic_exchange(&s->adopt, 0);
        int v = ictl(s, P_SAMPLE, 0, 0, SOUND_MAX);
        if (a >= OSC_COUNT && v > SOUND_SAMPLE && v - 1 == a) {
            int cur = sound_slot_of_sample(s);
            if (cur != a) { s->prev_own_slot = cur; set_sample_slot(s, a); }
        }
        atomic_store(&s->want_is_num, v > SOUND_SAMPLE);
    }
    atomic_store(&s->want_slot, sound_slot(s));

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

#define STATE_VERSION 2
#define STATE_MAX_BYTES (24u << 20) /* skip absurdly large files */

static LV2_State_Status state_save(LV2_Handle h, LV2_State_Store_Function store,
                                   LV2_State_Handle sh, uint32_t flags,
                                   const LV2_Feature* const* features)
{
    (void)flags; (void)features;
    KeySampler* s = h;
    int32_t version = STATE_VERSION, num = (int32_t)sound_slot(s);
    store(sh, s->k_version, &version, sizeof version, s->u_int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    store(sh, s->k_number, &num, sizeof num, s->u_int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    if (num >= OSC_COUNT) {
        char path[PATH_LEN];
        rec_path(s, num, path);
        pthread_mutex_lock(&s->file_lock);
        size_t len;
        void* data = read_file(path, &len);
        if (data && len <= STATE_MAX_BYTES)
            store(sh, s->k_data, data, len, s->u_chunk, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        free(data);
        pthread_mutex_unlock(&s->file_lock);
    }
    return LV2_STATE_SUCCESS;
}

/* Hosts with LV2 state (not the S2400) get the recording back if the card lost it. */
static LV2_State_Status state_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                      LV2_State_Handle sh, uint32_t flags,
                                      const LV2_Feature* const* features)
{
    (void)flags; (void)features;
    KeySampler* s = h;
    size_t size; uint32_t type, fl;
    const void* v = retrieve(sh, s->k_version, &size, &type, &fl);
    if (!v || type != s->u_int || *(const int32_t*)v < 2) return LV2_STATE_SUCCESS;
    v = retrieve(sh, s->k_number, &size, &type, &fl);
    int num = (v && type == s->u_int) ? *(const int32_t*)v : 0;
    const void* data = retrieve(sh, s->k_data, &size, &type, &fl);
    if (num < OSC_COUNT || num > MAX_SAMPLE || !data || type != s->u_chunk || size <= 44) return LV2_STATE_SUCCESS;
    char path[PATH_LEN];
    struct stat st;
    rec_path(s, num, path);
    pthread_mutex_lock(&s->file_lock);
    mkdir(s->rec_dir, 0755);
    if (stat(path, &st) != 0) write_file_atomic(path, data, size); /* never overwrite: numbers are unique */
    pthread_mutex_unlock(&s->file_lock);
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
