/*
 * n808_glue.c —— Node/Electron(WASM) 专用薄胶水层
 * ---------------------------------------------------------------------------
 * 只做三件事，全部复用 core808 的既有逻辑，不复制任何 DSP：
 *   1) 把 UI 需要的状态打包成一段固定布局的二进制快照（Worklet -> 主线程）；
 *   2) 工程字节的读取/写回（浏览器里没有文件系统，主进程负责落盘）；
 *   3) 一个稳定的操作码入口 n808_api()，让 JS 不依赖 core808 的枚举编号。
 *
 * 注意：快照布局必须与 app/snapshot.js 中的常量保持一致。
 */
#include "core808.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* 快照                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct {
    int32_t status[8];      /* 0   playing,pattern,step,songPos,loopCount,xruns,frames,blocks */
    float   cpu;            /* 32  CPU 负载 0..1                                            */
    float   meters[18];     /* 36  16 轨电平 + masterL + masterR                            */
    int32_t meta[8];        /* 108 len,swing,curPattern,songOn,patternCount,tsNum,tsDen,songLen */
    float   params[103];    /* 140 bpm,master[6],track[16][6]                               */
    uint8_t steps[10240];   /* 552 16*64*10                                                 */
    uint8_t flags[64];      /* 10792 16*4                                                   */
} n808_snap;

int n808_snap_size(void) { return (int)sizeof(n808_snap); }
int n808_project_size(void) { return (int)sizeof(c808_project); }
int n808_status_size(void) { return (int)sizeof(c808_status); }

void n808_pack_snapshot(c808_engine* e, int pat, unsigned frames, unsigned blocks, void* out)
{
    n808_snap* s = (n808_snap*)out;
    c808_status st;
    int t;
    if (!e || !out) return;
    memset(s, 0, sizeof(*s));
    c808_get_status(e, &st);
    s->status[0] = st.playing;
    s->status[1] = st.pattern;
    s->status[2] = st.step;
    s->status[3] = st.songPos;
    s->status[4] = st.loopCount;
    s->status[5] = st.xruns;
    s->status[6] = (int32_t)frames;
    s->status[7] = (int32_t)blocks;
    s->cpu = st.cpuLoad;
    for (t = 0; t < C808_TRACKS; ++t) s->meters[t] = st.meter[t];
    s->meters[16] = st.masterL;
    s->meters[17] = st.masterR;

    c808_snapshot(e, pat, s->meta, s->params, s->steps, s->flags);
    s->meta[7] = c808_get_song_len(e);
}

/* ------------------------------------------------------------------------- */
/* 稳定操作码（JS 侧只认这些编号，与 core808 内部枚举解耦）                   */
/* ------------------------------------------------------------------------- */
enum {
    N808_OP_STEP_ON = 1,
    N808_OP_STEP_VEL,
    N808_OP_STEP_ACCENT,
    N808_OP_STEP_PROB,
    N808_OP_STEP_RATCHET,
    N808_OP_STEP_RATE,
    N808_OP_STEP_COND,
    N808_OP_STEP_MICRO,
    N808_OP_TRACK_PARAM,
    N808_OP_TRACK_LEN,
    N808_OP_MUTE,
    N808_OP_SOLO,
    N808_OP_MASTER,
    N808_OP_BPM,
    N808_OP_TIMESIG,
    N808_OP_PATTERN_LEN,
    N808_OP_PATTERN_SWING,
    N808_OP_SELECT_PATTERN,
    N808_OP_SONG_ON,
    N808_OP_SONG_LEN,
    N808_OP_SONG_ENTRY,
    N808_OP_PATTERN_COUNT,
    N808_OP_CLEAR_PATTERN,
    N808_OP_CLEAR_TRACK,
    N808_OP_COPY_PATTERN,
    N808_OP_PLAY,
    N808_OP_STOP,
    N808_OP_STOP_RESET,
    N808_OP_PANIC,
    N808_OP_NOTE_ON,
    N808_OP_CLOCK
};

int n808_api(c808_engine* e, int op, int a, int b, int c, int n, float f)
{
    if (!e) return -1;
    switch (op) {
    case N808_OP_STEP_ON:      return c808_set_step_on(e, a, b, c, n);
    case N808_OP_STEP_VEL:     return c808_set_step_vel(e, a, b, c, n);
    case N808_OP_STEP_ACCENT:  return c808_set_step_accent(e, a, b, c, n);
    case N808_OP_STEP_PROB:    return c808_set_step_prob(e, a, b, c, n);
    case N808_OP_STEP_RATCHET: return c808_set_step_ratchet(e, a, b, c, n);
    case N808_OP_STEP_RATE:    return c808_set_step_rate(e, a, b, c, n);
    case N808_OP_STEP_COND:    return c808_set_step_cond(e, a, b, c, n, (int)f);
    case N808_OP_STEP_MICRO:   return c808_set_step_micro(e, a, b, c, n);
    case N808_OP_TRACK_PARAM:  return c808_set_track_param(e, a, b, f);
    case N808_OP_TRACK_LEN:    return c808_set_track_len(e, a, b);
    case N808_OP_MUTE:         return c808_set_mute(e, a, n);
    case N808_OP_SOLO:         return c808_set_solo(e, a, n);
    case N808_OP_MASTER:       return c808_set_master(e, a, f);
    case N808_OP_BPM:          return c808_set_bpm(e, f);
    case N808_OP_TIMESIG:      return c808_set_timesig(e, a, b);
    case N808_OP_PATTERN_LEN:  return c808_set_pattern_len(e, a, b);
    case N808_OP_PATTERN_SWING:return c808_set_pattern_swing(e, a, b);
    case N808_OP_SELECT_PATTERN: return c808_select_pattern(e, a);
    case N808_OP_SONG_ON:      return c808_set_song_on(e, a);
    case N808_OP_SONG_LEN:     return c808_set_song_len(e, a);
    case N808_OP_SONG_ENTRY:   return c808_set_song_entry(e, a, b, c);
    case N808_OP_PATTERN_COUNT:return c808_set_pattern_count(e, a);
    case N808_OP_CLEAR_PATTERN:return c808_clear_pattern(e, a);
    case N808_OP_CLEAR_TRACK:  return c808_clear_track(e, a, b);
    case N808_OP_COPY_PATTERN: return c808_copy_pattern(e, a, b);
    case N808_OP_PLAY:         return c808_transport_play(e);
    case N808_OP_STOP:         return c808_transport_stop(e);
    case N808_OP_STOP_RESET:   return c808_transport_stop_reset(e);
    case N808_OP_PANIC:        return c808_panic(e);
    case N808_OP_NOTE_ON:      return c808_note_on(e, a, b, c);
    case N808_OP_CLOCK:        return c808_set_clock_source(e, a);
    default: return -2;
    }
}

/* ------------------------------------------------------------------------- */
/* 工程字节（与 core808_io.c 的文件格式完全一致：20 字节头 + 结构体）        */
/* ------------------------------------------------------------------------- */
static unsigned n808_fnv32(const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void n808_put_u32(unsigned char* b, unsigned v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
}

static unsigned n808_get_u32(const unsigned char* b)
{
    return (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
}

int n808_project_out(c808_engine* e, void* out, int cap)
{
    const c808_project* p;
    unsigned char* o = (unsigned char*)out;
    int size = (int)sizeof(c808_project);
    if (!e || !out || cap < size + 20) return -1;
    p = c808_project_ro(e);
    if (!p) return -2;
    memcpy(o, "C808PJ01", 8);
    n808_put_u32(o + 8, 1);
    n808_put_u32(o + 12, (unsigned)size);
    n808_put_u32(o + 16, n808_fnv32(p, sizeof(*p)));
    memcpy(o + 20, p, sizeof(*p));
    return size + 20;
}

int n808_project_in(c808_engine* e, const void* data, int len)
{
    const unsigned char* b = (const unsigned char*)data;
    c808_project* sh;
    unsigned ver, size, sum;
    if (!e || !data) return -2;
    if (len < 20) return -4;
    if (memcmp(b, "C808PJ01", 8) != 0) return -3;
    ver = n808_get_u32(b + 8);
    size = n808_get_u32(b + 12);
    sum = n808_get_u32(b + 16);
    if (ver != 1 || size != (unsigned)sizeof(c808_project)) return -5;
    if (len < (int)(20 + size)) return -6;
    sh = c808_project_shadow(e);
    if (!sh) return -6;
    memcpy(sh, b + 20, sizeof(*sh));
    if (n808_fnv32(sh, sizeof(*sh)) != sum) return -7;
    if (sh->magic != 0x50383038u) return -8;
    sh->bpm = sh->bpm < 20.0f ? 120.0f : (sh->bpm > 400.0f ? 400.0f : sh->bpm);
    sh->currentPattern = sh->currentPattern < 0 ? 0 :
        (sh->currentPattern >= C808_PATTERNS ? C808_PATTERNS - 1 : sh->currentPattern);
    c808_activate_shadow(e);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Song 链读取（供 SONG CHAIN EDITOR 弹窗）                                   */
/* 布局：[int32 count][count * (uint16 pat, uint16 reps)]                     */
/* ------------------------------------------------------------------------- */
int n808_song_out(c808_engine* e, void* out, int cap)
{
    unsigned char* o = (unsigned char*)out;
    int i, n;
    if (!e || !out || cap < 4) return -1;
    n = c808_get_song_len(e);
    if (n < 0) n = 0;
    if (4 + n * 4 > cap) return -2;
    n808_put_u32(o, (unsigned)n);
    for (i = 0; i < n; ++i) {
        int pat = 0, reps = 1;
        c808_get_song_entry(e, i, &pat, &reps);
        o[4 + i * 4 + 0] = (unsigned char)(pat & 0xff);
        o[4 + i * 4 + 1] = (unsigned char)((pat >> 8) & 0xff);
        o[4 + i * 4 + 2] = (unsigned char)(reps & 0xff);
        o[4 + i * 4 + 3] = (unsigned char)((reps >> 8) & 0xff);
    }
    return 4 + n * 4;
}
