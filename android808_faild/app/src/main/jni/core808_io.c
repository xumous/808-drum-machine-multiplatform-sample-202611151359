/*
 * core808_io.c —— WAV / MIDI 导出、工程存取、Windows MIDI 输入
 * ---------------------------------------------------------------------------
 * 所有离线渲染都通过“新建一个独立引擎 + 复制工程 + 激活槽位”完成，
 * 走的是与实时播放完全相同的 c808_render() 路径，保证导出与试听一致。
 */
#include "core808.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

/* UTF-8 路径安全的 fopen：Windows 上转宽字符后 _wfopen */
static FILE* fopen_u8(const char* path, const char* mode)
{
#ifdef _WIN32
    wchar_t wp[MAX_PATH * 2];
    wchar_t wm[16];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, (int)(sizeof(wp) / sizeof(wp[0])));
    int m = MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16);
    if (n <= 0 || m <= 0) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

/* ========================================================================= */
/* 小工具                                                                     */
/* ========================================================================= */
static void put_u16(FILE* f, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    fwrite(b, 1, 2, f);
}

static void put_u24(FILE* f, int v)
{
    unsigned char b[3];
    if (v > 8388607) v = 8388607;
    if (v < -8388608) v = -8388608;
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    fwrite(b, 1, 3, f);
}

static void put_u32(FILE* f, unsigned v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
    fwrite(b, 1, 4, f);
}

static unsigned fnv32(const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ========================================================================= */
/* 把源工程复制到一个临时引擎（shadow -> activate），用于离线渲染             */
/* ========================================================================= */
static c808_engine* make_offline_engine(c808_engine* src, int mode, int songOn)
{
    c808_engine* t;
    c808_project* sh;
    const c808_project* sp;
    if (!src) return NULL;
    t = c808_create((double)C808_SR_DEFAULT);
    if (!t) return NULL;
    sp = c808_project_ro(src);
    sh = c808_project_shadow(t);
    *sh = *sp;                      /* 3.4MB 工程拷贝，发生在 UI/工作线程 */
    sh->songOn = songOn;
    if (mode == 0) {
        sh->currentPattern = sp->currentPattern;
    }
    c808_activate_slot(t, 1);
    c808_panic(t);
    return t;
}

static double samples_per_step(const c808_project* p, double sr)
{
    double bpm = p->bpm;
    if (bpm < 20.0) bpm = 20.0;
    if (bpm > 400.0) bpm = 400.0;
    return 60.0 / bpm / 4.0 * sr;
}

/* pattern/song 总步数（用于计算导出长度） */
static long long total_steps(const c808_project* p, int mode, int loops)
{
    int i, j;
    long long n = 0;
    if (mode == 0) {
        int pat = p->currentPattern;
        int len = p->patterns[pat].len;
        if (len < 1) len = 1;
        if (loops < 1) loops = 1;
        return (long long)len * loops;
    }
    if (p->songLen <= 0) return (long long)p->patterns[p->currentPattern].len;
    for (i = 0; i < p->songLen; ++i) {
        int pat = p->song[i].pat;
        int reps = p->song[i].reps ? p->song[i].reps : 1;
        int len = p->patterns[pat].len;
        if (len < 1) len = 1;
        for (j = 0; j < reps; ++j) n += len;
    }
    return n;
}

static const char* track_suffix(int t)
{
    static const char* s[C808_TRACKS] = {
        "kick", "snare", "clap", "rim", "tomlo", "tommid", "tomhi", "conga",
        "cowbell", "shaker", "tamb", "crash", "ride", "hatopen", "hatclosed", "clave"
    };
    return (t >= 0 && t < C808_TRACKS) ? s[t] : "track";
}

/* ========================================================================= */
/* WAV 导出                                                                   */
/* ========================================================================= */
static int render_to_wav(c808_engine* eng, const char* path,
                         long long steps, double stepSamples, int tailSec)
{
    FILE* f;
    unsigned dataBytes = 0;
    unsigned sr;
    long long totalFrames, done;
    float bufL[512], bufR[512];
    int i;

    sr = (unsigned)(C808_SR_DEFAULT + 0.5);
    f = fopen_u8(path, "wb");
    if (!f) return -1;
    totalFrames = (long long)(steps * stepSamples) + (long long)tailSec * (long long)sr;
    if (totalFrames < 1) totalFrames = 1;

    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36u + (unsigned)(totalFrames * 6));
    fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16);
    put_u16(f, 1);          /* PCM */
    put_u16(f, 2);          /* stereo */
    put_u32(f, sr);
    put_u32(f, sr * 6);     /* byte rate */
    put_u16(f, 6);          /* block align */
    put_u16(f, 24);         /* bits */
    fwrite("data", 1, 4, f);
    put_u32(f, 0);          /* 占位，末尾回填 */

    c808_transport_play(eng);
    done = 0;
    while (done < totalFrames) {
        int n = 512;
        if ((long long)n > totalFrames - done) n = (int)(totalFrames - done);
        if (done >= (long long)(steps * stepSamples)) {
            /* 尾音阶段：停止音序器，让 voice 自然衰减 */
            c808_transport_stop(eng);
        }
        c808_render(eng, bufL, bufR, n);
        for (i = 0; i < n; ++i) {
            put_u24(f, (int)(bufL[i] * 8388607.0f));
            put_u24(f, (int)(bufR[i] * 8388607.0f));
        }
        dataBytes += (unsigned)(n * 6);
        done += n;
    }

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36u + dataBytes);
    fseek(f, 4 + 4 + 4 + 4 + 4 + 2 + 2 + 4 + 4 + 2 + 2 + 4, SEEK_SET);
    put_u32(f, dataBytes);
    fclose(f);
    return 0;
}

int c808_export_wav(c808_engine* e, const char* path, int mode, int loops,
                    int includeTailSeconds, int stems)
{
    const c808_project* src;
    c808_engine* t;
    long long steps;
    double sps;
    int rc = 0;
    int tr;
    char tmp[1024];

    if (!e || !path) return -2;
    src = c808_project_ro(e);
    steps = total_steps(src, mode, loops);
    sps = samples_per_step(src, C808_SR_DEFAULT);
    if (includeTailSeconds < 0) includeTailSeconds = 0;
    if (includeTailSeconds > 30) includeTailSeconds = 30;

    if (!stems) {
        t = make_offline_engine(e, mode, mode == 1);
        if (!t) return -1;
        rc = render_to_wav(t, path, steps, sps, includeTailSeconds);
        c808_destroy(t);
        return rc;
    }

    /* 分轨导出：每轨单独渲染一个 WAV，主效果旁路，保留轨道电平/声像 */
    for (tr = 0; tr < C808_TRACKS; ++tr) {
        c808_project* sh;
        int i;
        t = make_offline_engine(e, mode, mode == 1);
        if (!t) return -1;
        sh = c808_project_shadow(t);
        for (i = 0; i < C808_TRACKS; ++i) sh->mute[i] = (i == tr) ? 0 : 1;
        sh->master[C808_M_DRIVE] = 0.0f;
        sh->master[C808_M_HP] = 0.0f;
        sh->master[C808_M_LP] = 1.0f;
        sh->master[C808_M_RESO] = 0.0f;
        sh->master[C808_M_VOLUME] = 1.0f;
        c808_activate_shadow(t);
        sprintf(tmp, "%s", path);
        {
            char* dot = strrchr(tmp, '.');
            if (dot && (dot - tmp) < (int)sizeof(tmp) - 8) *dot = '\0';
        }
        sprintf(tmp + strlen(tmp), "_%s.wav", track_suffix(tr));
        rc |= render_to_wav(t, tmp, steps, sps, includeTailSeconds);
        c808_destroy(t);
    }
    return rc;
}

/* ========================================================================= */
/* MIDI 导出（SMF format 0，PPQ 960，鼓通道 10）                              */
/* ========================================================================= */
typedef struct { int tick; int note; int vel; int on; } mev;

static void midi_vlq(FILE* f, unsigned v)
{
    unsigned char b[4];
    int n = 0, i;
    b[n++] = (unsigned char)(v & 0x7f);
    while ((v >>= 7) != 0) b[n++] = (unsigned char)(0x80 | (v & 0x7f));
    for (i = n - 1; i >= 0; --i) fputc(b[i], f);
}

static void midi_sorted_flush(FILE* f, mev* ev, int count, int* lastTick)
{
    int i, j;
    for (i = 1; i < count; ++i) {
        mev key = ev[i];
        j = i - 1;
        while (j >= 0 && (ev[j].tick > key.tick ||
               (ev[j].tick == key.tick && ev[j].on < key.on))) {
            ev[j + 1] = ev[j];
            j--;
        }
        ev[j + 1] = key;
    }
    for (i = 0; i < count; ++i) {
        int delta = ev[i].tick - *lastTick;
        if (delta < 0) delta = 0;
        midi_vlq(f, (unsigned)delta);
        if (ev[i].on) {
            fputc(0x99, f);                 /* note on ch10 */
            fputc(ev[i].note & 0x7f, f);
            fputc(ev[i].vel & 0x7f, f);
        } else {
            fputc(0x89, f);
            fputc(ev[i].note & 0x7f, f);
            fputc(0, f);
        }
        *lastTick = ev[i].tick;
    }
}

int c808_export_midi(c808_engine* e, const char* path, int mode, int loops)
{
    const c808_project* p;
    FILE* f;
    long long total, idx;
    int lastTick = 0;
    unsigned char hdr[14];
    unsigned trkLenPos;
    long trkStart;
    long long stepIndex = 0;
    mev ev[C808_TRACKS * 8 * 2];
    int i;

    if (!e || !path) return -2;
    p = c808_project_ro(e);
    total = total_steps(p, mode, loops);

    f = fopen_u8(path, "wb");
    if (!f) return -1;

    fwrite("MThd", 1, 4, f);
    put_u32(f, 6);
    put_u16(f, 0);      /* format 0 */
    put_u16(f, 1);      /* 1 track */
    put_u16(f, 960);    /* PPQ */
    (void)hdr;

    fwrite("MTrk", 1, 4, f);
    trkLenPos = (unsigned)ftell(f);
    put_u32(f, 0);      /* 占位 */
    trkStart = ftell(f);

    /* Tempo */
    midi_vlq(f, 0);
    fputc(0xFF, f); fputc(0x51, f); fputc(3, f);
    {
        unsigned us = (unsigned)(60000000.0 / (double)(p->bpm > 20.0f ? p->bpm : 120.0f));
        fputc((us >> 16) & 0xff, f);
        fputc((us >> 8) & 0xff, f);
        fputc(us & 0xff, f);
    }
    /* 拍号 */
    midi_vlq(f, 0);
    fputc(0xFF, f); fputc(0x58, f); fputc(4, f);
    fputc(p->tsNum > 0 ? p->tsNum : 4, f);
    {
        int den = p->tsDen > 0 ? p->tsDen : 4;
        int pow2 = 0;
        while (den > 1) { den >>= 1; pow2++; }
        fputc(pow2, f);
    }
    fputc(24, f);
    fputc(8, f);
    /* 轨名 */
    midi_vlq(f, 0);
    fputc(0xFF, f); fputc(0x03, f);
    midi_vlq(f, 4); fwrite("808 ", 1, 4, f);

    for (idx = 0; idx < total; ++idx) {
        int patIdx;
        int localStep;
        int len;
        int track;
        double sps = samples_per_step(p, C808_SR_DEFAULT);
        (void)stepIndex;

        if (mode == 1) {
            /* 展开 song 链，定位当前 pattern 与 pattern 内的步号 */
            long long acc = 0;
            int s;
            patIdx = p->patterns[p->currentPattern].len ? p->currentPattern : 0;
            localStep = 0;
            for (s = 0; s < p->songLen; ++s) {
                int reps = p->song[s].reps ? p->song[s].reps : 1;
                int plen = p->patterns[p->song[s].pat].len;
                long long span;
                int r;
                if (plen < 1) plen = 1;
                span = (long long)plen * reps;
                if (idx < acc + span) {
                    patIdx = p->song[s].pat;
                    localStep = (int)((idx - acc) % plen);
                    break;
                }
                acc += span;
                (void)r;
            }
        } else {
            patIdx = p->currentPattern;
            len = p->patterns[patIdx].len;
            if (len < 1) len = 1;
            localStep = (int)(idx % len);
        }
        {
            const c808_pattern* pat = &p->patterns[patIdx];
            int plen = pat->len < 1 ? 1 : pat->len;
            double swingTicks = 0.0;
            if (pat->swing > 0 && (localStep & 1)) {
                swingTicks = 240.0 * 0.5 * (double)pat->swing / 100.0;
            }
            for (track = 0; track < C808_TRACKS; ++track) {
                int tlen = p->trackLen[track] < 1 ? 1 : p->trackLen[track];
                int s = localStep % tlen;
                int n, k;
                int count = 0;
                int condOk = 1;
                if (!pat->on[track][s]) continue;
                switch (pat->cond[track][s]) {
                case C808_COND_1_2: condOk = ((idx / plen) % 2) == 0; break;
                case C808_COND_1_3: condOk = ((idx / plen) % 3) == 0; break;
                case C808_COND_1_4: condOk = ((idx / plen) % 4) == 0; break;
                case C808_COND_FILL: condOk = ((idx / plen) % 4) == 3; break;
                case C808_COND_NOTFILL: condOk = ((idx / plen) % 4) != 3; break;
                case C808_COND_EVERY_N:
                    condOk = pat->condN[track][s] > 0 &&
                             ((idx / plen) % pat->condN[track][s]) == 0;
                    break;
                default: break;
                }
                if (!condOk) continue;
                n = pat->ratchet[track][s];
                if (n < 1) n = 1;
                if (n > 8) n = 8;
                for (k = 0; k < n; ++k) {
                    double microTicks = (double)pat->micro[track][s] / sps * 240.0;
                    double spacing = 240.0 * (double)pat->rate[track][s] / 100.0 / (double)n;
                    double tickD = (double)idx * 240.0 + swingTicks + microTicks + spacing * k;
                    int vel = pat->vel[track][s];
                    if (vel < 1) vel = 1;
                    if (pat->accent[track][s]) {
                        vel = (int)(vel * (1.0 + p->master[C808_M_ACCENT] * 0.5));
                        if (vel > 127) vel = 127;
                    }
                    ev[count].tick = (int)(tickD + 0.5);
                    ev[count].note = p->midiNote[track];
                    ev[count].vel = vel;
                    ev[count].on = 1;
                    count++;
                    ev[count].tick = (int)(tickD + 0.5) + 6;
                    ev[count].note = p->midiNote[track];
                    ev[count].vel = 0;
                    ev[count].on = 0;
                    count++;
                }
                if (count > 0) midi_sorted_flush(f, ev, count, &lastTick);
            }
        }
    }

    midi_vlq(f, 0);
    fputc(0xFF, f); fputc(0x2F, f); fputc(0, f);
    {
        long trkEnd = ftell(f);
        unsigned len = (unsigned)(trkEnd - trkStart);
        fseek(f, (long)trkLenPos, SEEK_SET);
        put_u32(f, len);
    }
    fclose(f);
    (void)i;
    return 0;
}

/* ========================================================================= */
/* 工程保存 / 加载                                                            */
/* ========================================================================= */
int c808_save_project(c808_engine* e, const char* path)
{
    const c808_project* p;
    FILE* f;
    unsigned sum;
    if (!e || !path) return -2;
    p = c808_project_ro(e);
    sum = fnv32(p, sizeof(*p));
    f = fopen_u8(path, "wb");
    if (!f) return -1;
    fwrite("C808PJ01", 1, 8, f);
    put_u32(f, 1);
    put_u32(f, (unsigned)sizeof(*p));
    put_u32(f, sum);
    fwrite(p, 1, sizeof(*p), f);
    fclose(f);
    return 0;
}

int c808_load_project(c808_engine* e, const char* path)
{
    FILE* f;
    char magic[8];
    unsigned ver, size, sum;
    c808_project* sh;
    if (!e || !path) return -2;
    f = fopen_u8(path, "rb");
    if (!f) return -1;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "C808PJ01", 8) != 0) {
        fclose(f);
        return -3;
    }
    if (fread(&ver, 4, 1, f) != 1 || fread(&size, 4, 1, f) != 1 || fread(&sum, 4, 1, f) != 1) {
        fclose(f);
        return -4;
    }
    if (ver != 1 || size != (unsigned)sizeof(c808_project)) {
        fclose(f);
        return -5;
    }
    sh = c808_project_shadow(e);
    if (fread(sh, 1, sizeof(*sh), f) != sizeof(*sh)) {
        fclose(f);
        return -6;
    }
    fclose(f);
    if (fnv32(sh, sizeof(*sh)) != sum) return -7;
    if (sh->magic != 0x50383038u) return -8;
    sh->bpm = sh->bpm < 20.0f ? 120.0f : (sh->bpm > 400.0f ? 400.0f : sh->bpm);
    sh->currentPattern = sh->currentPattern < 0 ? 0 :
        (sh->currentPattern >= C808_PATTERNS ? C808_PATTERNS - 1 : sh->currentPattern);
    c808_activate_shadow(e);
    return 0;
}

/* ========================================================================= */
/* Windows MIDI 输入（winmm），回调线程只做无锁入队                           */
/* ========================================================================= */
#ifdef _WIN32
static HMIDIIN g_midi_handle = NULL;

static void CALLBACK c808_midi_cb(HMIDIIN h, UINT msg, DWORD_PTR inst,
                                  DWORD_PTR p1, DWORD_PTR p2)
{
    c808_engine* e = (c808_engine*)inst;
    unsigned char status;
    (void)h; (void)p2;
    if (msg != MIM_DATA || !e) return;
    status = (unsigned char)(p1 & 0xff);
    if (status >= 0xF8) {
        switch (status) {
        case 0xF8: c808_push_midi(e, 4, 0, 0, 0); break; /* CLOCK */
        case 0xFA: c808_push_midi(e, 5, 0, 0, 0); break; /* START */
        case 0xFC: c808_push_midi(e, 6, 0, 0, 0); break; /* STOP */
        case 0xFB: c808_push_midi(e, 7, 0, 0, 0); break; /* CONTINUE */
        default: break;
        }
        return;
    }
    {
        unsigned char type = status & 0xF0;
        unsigned char d1 = (unsigned char)((p1 >> 8) & 0x7f);
        unsigned char d2 = (unsigned char)((p1 >> 16) & 0x7f);
        if (type == 0x90 && d2 > 0) {
            c808_push_midi(e, 1, d1, d2, 0);
        } else if (type == 0xC0) {
            c808_push_midi(e, 3, d1, 0, 0);
        } else if (type == 0xB0) {
            c808_push_midi(e, 2, d1, d2, 0);
        }
    }
}

int c808_midi_device_count(void)
{
    return (int)midiInGetNumDevs();
}

int c808_midi_device_name(int index, char* out, int cap)
{
    MIDIINCAPS caps;
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (midiInGetDevCaps((UINT_PTR)index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return -1;
    strncpy(out, (const char*)caps.szPname, (size_t)cap - 1);
    out[cap - 1] = 0;
    return 0;
}

int c808_midi_start(c808_engine* e, int deviceIndex)
{
    HMIDIIN h = NULL;
    MMRESULT r;
    if (!e) return -1;
    if (g_midi_handle) c808_midi_stop(e);
    if (midiInGetNumDevs() == 0) return -2;
    if (deviceIndex < 0 || (UINT)deviceIndex >= midiInGetNumDevs()) deviceIndex = 0;
    r = midiInOpen(&h, (UINT)deviceIndex, (DWORD_PTR)&c808_midi_cb,
                   (DWORD_PTR)e, CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) return -3;
    g_midi_handle = h;
    midiInStart(h);
    return 0;
}

void c808_midi_stop(c808_engine* e)
{
    (void)e;
    if (!g_midi_handle) return;
    midiInStop(g_midi_handle);
    midiInReset(g_midi_handle);
    midiInClose(g_midi_handle);
    g_midi_handle = NULL;
}
#else
int c808_midi_device_count(void) { return 0; }
int c808_midi_device_name(int index, char* out, int cap)
{ (void)index; if (out && cap > 0) out[0] = 0; return -1; }
int c808_midi_start(c808_engine* e, int deviceIndex)
{ (void)e; (void)deviceIndex; return -2; }
void c808_midi_stop(c808_engine* e) { (void)e; }
#endif

