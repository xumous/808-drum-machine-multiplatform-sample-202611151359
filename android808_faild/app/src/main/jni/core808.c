/*
 * core808.c —— 实时 DSP / 16 声部模拟合成 / 采样级 16-64 步音序器
 * ---------------------------------------------------------------------------
 * 音频线程约束（本文件中的 c808_render 及其调用链）：
 *   - 不调用 malloc/free、不获取互斥锁、不做文件或设备 I/O；
 *   - 所有数组在 c808_create() 中一次性分配；
 *   - UI 参数通过无锁 SPSC 环形队列进入音频线程，在块首统一应用；
 *   - 时钟只使用“累计渲染采样数”，浮点步长误差通过整数+余数累加消除。
 */
#include "core808.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifndef C808_M_PI
#define C808_M_PI 3.14159265358979323846f
#endif

/* ========================================================================= */
/* 快速数学：正弦表 + exp2 + tanh（跨平台确定，不依赖 libm）                  */
/* ========================================================================= */
#define SIN_BITS 12
#define SIN_SIZE (1 << SIN_BITS)
#define SIN_MASK (SIN_SIZE - 1)

typedef struct {
    float sinTab[SIN_SIZE + 1];
    float sr;
} c808_math;

/* 多项式 sin，仅在初始化正弦表时使用（参数范围 0..pi/2） */
static float poly_sin(float x)
{
    /* minimax 近似，误差 < 1e-5，足够生成查找表 */
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.008333333f +
           x2 * (-0.0001984127f + x2 * 0.000002755732f))));
}

static void math_init(c808_math* m)
{
    int i;
    for (i = 0; i <= SIN_SIZE; ++i) {
        float t = (float)i / (float)SIN_SIZE;   /* 0..1 圈 */
        float x = t * 2.0f * C808_M_PI;
        float s;
        if (x <= C808_M_PI * 0.5f) {
            s = poly_sin(x);
        } else if (x <= C808_M_PI) {
            s = poly_sin(C808_M_PI - x);
        } else if (x <= C808_M_PI * 1.5f) {
            s = -poly_sin(x - C808_M_PI);
        } else {
            s = -poly_sin(2.0f * C808_M_PI - x);
        }
        m->sinTab[i] = s;
    }
}

/* 相位为“圈”单位 [0,1)，线性插值查表 */
static inline float fast_sin(const c808_math* m, float phase)
{
    float x;
    int idx;
    float frac;
    phase -= (float)(int)phase;       /* 归一到 [0,1) */
    if (phase < 0.0f) phase += 1.0f;
    x = phase * (float)SIN_SIZE;
    idx = (int)x;
    frac = x - (float)idx;
    return m->sinTab[idx] + (m->sinTab[idx + 1] - m->sinTab[idx]) * frac;
}

static inline float fast_cos(const c808_math* m, float phase)
{
    return fast_sin(m, phase + 0.25f);
}

/* 2^x 近似：整数部分用指数位，小数部分 5 阶多项式。误差 < 3e-5。 */
static inline float fast_exp2(float x)
{
    int xi;
    float f, p;
    union { uint32_t u; float f; } bits;
    if (x < -60.0f) x = -60.0f;
    if (x > 60.0f) x = 60.0f;
    xi = (int)x;
    if ((float)xi > x) xi -= 1;       /* floor */
    f = x - (float)xi;
    p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f +
        f * (0.00961813f + f * 0.00133336f))));
    bits.u = (uint32_t)(xi + 127) << 23;
    return p * bits.f;
}

static inline float fast_exp(float x)
{
    return fast_exp2(x * 1.4426950408889634f);
}

/* 指数衰减系数：每个采样乘一次，等价 exp(-1/(tau*sr)) */
static inline float decay_coef(float tau_sec, float sr)
{
    if (tau_sec < 1.0f / sr) tau_sec = 1.0f / sr;
    return fast_exp(-1.0f / (tau_sec * sr));
}

static inline float fast_tanh(float x)
{
    float x2;
    if (x > 4.0f) return 1.0f;
    if (x < -4.0f) return -1.0f;
    x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* tan(pi*fc/sr) 的多项式近似，参数限制在稳定范围 */
static inline float fast_tan(float x)
{
    float x2;
    if (x < 0.0001f) return 0.0001f;
    if (x > 1.45f) x = 1.45f;          /* 对应 ~0.46*sr，避免爆炸 */
    x2 = x * x;
    return x + x * x2 * (0.3333333f + x2 * (0.1333333f + x2 * 0.0539683f));
}

/* xorshift32 噪声，范围 [-1,1) */
static inline float voice_noise(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* ========================================================================= */
/* 状态变量滤波器（TPT SVF），mode: 0=LP 1=BP 2=HP                            */
/* ========================================================================= */
typedef struct { float ic1, ic2; } c808_svf;

static inline float svf_process(c808_svf* s, float x, float g, float k, int mode)
{
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = x - s->ic2;
    float v1 = a1 * s->ic1 + a2 * v3;
    float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    if (mode == 0) return v2;
    if (mode == 1) return v1;
    return x - k * v1 - v2;
}

/* ========================================================================= */
/* 运行时结构                                                                 */
/* ========================================================================= */
typedef struct {
    int active;
    int track;
    int type;
    uint64_t order;
    float phase[6];
    float env;          /* 主幅度包络 */
    float env2;         /* 辅助包络（噪声/点击/尾音） */
    float env3;         /* 第三包络（拍手爆发等） */
    float coef1, coef2, coef3;
    float f0;           /* 基频 */
    float pitchEnv;     /* 音高包络 */
    float pitchCoef;
    float vel;
    float accent;
    float aux;          /* 类型相关辅助量 */
    float choke;        /* 0=正常 1=正在被 choke */
    float g1, k1, g2, k2, gB, kB, gH, kH; /* 预计算滤波系数 */
    uint32_t rng;
    c808_svf f1, f2, f3;
} c808_voice_rt;

typedef struct {
    float p[C808_P_COUNT];       /* 平滑后的参数 */
    float target[C808_P_COUNT];  /* UI 目标值 */
    uint64_t nextStep;           /* 下一个网格采样位置（保留，当前使用全局网格） */
    int step;
    int pendCount;
    int pendIdx;
    uint64_t evTime[8];
    float evVel[8];
    uint8_t evAccent[8];
    float gainL, gainR;          /* 本块声像增益 */
} c808_track_rt;

struct c808_engine {
    c808_math math;
    float sr;

    /* 双工程槽：音频线程只读取 active，UI 写入 shadow 后原子激活 */
    c808_project projects[2];
    atomic_int slot;
    const c808_project* proj;

    /* 无锁命令队列（SPSC） */
    c808_cmd ring[C808_CMD_RING];
    atomic_uint ring_head;   /* 消费位置（音频线程） */
    atomic_uint ring_tail;   /* 生产位置（UI 线程） */

    /* MIDI 输入事件队列（winmm 回调线程 -> 音频线程） */
    struct { int32_t type, a, b, c; } midiRing[1024];
    atomic_uint midi_head, midi_tail;
    void* midi_handle;

    /* 音序器 */
    volatile int playing;
    volatile int64_t samplePos;
    uint64_t nextGrid;
    double gridFrac;
    double stepSamples;
    int globalStep;
    int curPattern;
    int pendingPattern;
    int loopCount;
    int songPos, songRep;
    uint32_t rng;
    uint64_t triggerCount;
    int midiClockCount;

    c808_voice_rt voices[C808_TRACKS][C808_VOICES_PER_TRACK];
    uint64_t voiceOrder;
    c808_track_rt tracks[C808_TRACKS];

    /* 主效果 */
    c808_svf hp[2], lp[2];
    float hpG, hpK, lpG, lpK;
    float dcx1[2], dcy1[2], satComp;
    float masterSm[C808_M_COUNT];
    float masterTarget[C808_M_COUNT];
    float meter[C808_TRACKS];
    float meterL, meterR;
    volatile float cpuLoad;
    volatile long xruns;
    volatile int anySolo;

    /* 由 c808_get_status 读取的原子快照 */
    volatile int uiStep;
    volatile int uiPattern;
    volatile int uiSongPos;
    volatile int uiLoop;
};

/* ========================================================================= */
/* 命令队列（单生产者/单消费者，无锁）                                        */
/* ========================================================================= */
static inline int ring_push(c808_engine* e, const c808_cmd* c)
{
    unsigned tail = atomic_load_explicit(&e->ring_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&e->ring_head, memory_order_acquire);
    if ((tail - head) >= C808_CMD_RING) return -1;
    e->ring[tail & (C808_CMD_RING - 1)] = *c;
    atomic_store_explicit(&e->ring_tail, tail + 1, memory_order_release);
    return 0;
}

static inline int ring_pop(c808_engine* e, c808_cmd* out)
{
    unsigned head = atomic_load_explicit(&e->ring_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&e->ring_tail, memory_order_acquire);
    if (head == tail) return 0;
    *out = e->ring[head & (C808_CMD_RING - 1)];
    atomic_store_explicit(&e->ring_head, head + 1, memory_order_release);
    return 1;
}

int c808_send_cmd(c808_engine* e, const c808_cmd* cmd)
{
    if (!e || !cmd) return -1;
    return ring_push(e, cmd);
}

static int push0(c808_engine* e, int type, int a, int b, int c, float f)
{
    c808_cmd cmd;
    cmd.type = type; cmd.a = a; cmd.b = b; cmd.c = c; cmd.f = f;
    return ring_push(e, &cmd);
}

/* ========================================================================= */
/* 参数钳制与映射                                                             */
/* ========================================================================= */
static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float f01(float v) { return clampf(v, 0.0f, 1.0f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float param_default(int track, int param)
{
    (void)track;
    switch (param) {
    case C808_P_TUNE:  return 0.5f;
    case C808_P_DECAY: return 0.5f;
    case C808_P_TONE:  return 0.5f;
    case C808_P_CHAR:  return 0.5f;
    case C808_P_LEVEL: return 0.8f;
    case C808_P_PAN:   return 0.5f;
    default: return 0.5f;
    }
}

static void track_runtime_init(c808_track_rt* rt, int track)
{
    int i;
    memset(rt, 0, sizeof(*rt));
    for (i = 0; i < C808_P_COUNT; ++i) {
        rt->p[i] = param_default(track, i);
        rt->target[i] = rt->p[i];
    }
    rt->gainL = 0.7071f;
    rt->gainR = 0.7071f;
}

/* ========================================================================= */
/* 默认工程                                                                   */
/* ========================================================================= */
void c808_init_default_project(c808_project* p)
{
    int t, s, i;
    memset(p, 0, sizeof(*p));
    p->magic = 0x50383038u; /* '808P' */
    p->version = 1;
    strcpy(p->name, "808 Factory");
    p->bpm = 120.0f;
    p->tsNum = 4;
    p->tsDen = 4;
    p->currentPattern = 0;
    p->patternCount = 4;
    p->songOn = 0;
    p->songLen = 1;
    p->song[0].pat = 0;
    p->song[0].reps = 4;

    for (i = 0; i < C808_PATTERNS; ++i) {
        p->patterns[i].len = 16;
        p->patterns[i].swing = 0;
        for (t = 0; t < C808_TRACKS; ++t) {
            for (s = 0; s < C808_STEPS; ++s) {
                p->patterns[i].vel[t][s] = 100;
                p->patterns[i].prob[t][s] = 100;
                p->patterns[i].ratchet[t][s] = 1;
                p->patterns[i].rate[t][s] = 100;
            }
        }
    }

    /* 演示节奏：经典 16 步 */
    {
        c808_pattern* q = &p->patterns[0];
        static const int kicks[4] = {0, 4, 8, 12};
        static const int snares[2] = {4, 12};
        static const int hats[16] = {0,2,4,6,8,10,12,14,0,2,4,6,8,10,12,14};
        for (i = 0; i < 4; ++i) {
            q->on[C808_KICK][kicks[i]] = 1;
            q->vel[C808_KICK][kicks[i]] = (i == 0) ? 127 : 104;
            q->accent[C808_KICK][kicks[i]] = (i == 0);
        }
        for (i = 0; i < 2; ++i) {
            q->on[C808_SNARE][snares[i]] = 1;
            q->vel[C808_SNARE][snares[i]] = 112;
        }
        for (i = 0; i < 16; ++i) {
            q->on[C808_HAT_CLOSED][hats[i]] = 1;
            q->vel[C808_HAT_CLOSED][hats[i]] = (i & 1) ? 58 : 80;
        }
        q->on[C808_HAT_OPEN][14] = 1;
        q->vel[C808_HAT_OPEN][14] = 96;
        q->on[C808_CLAP][12] = 1;
        q->vel[C808_CLAP][12] = 90;
    }

    for (t = 0; t < C808_TRACKS; ++t) {
        p->tune[t] = 0.5f;
        p->decay[t] = 0.5f;
        p->tone[t] = 0.5f;
        p->chara[t] = 0.5f;
        p->level[t] = 0.78f;
        p->pan[t] = 0.5f;
        p->mute[t] = 0;
        p->solo[t] = 0;
        p->trackLen[t] = 16;
    }
    /* 拍手尾音、开镲 choke 等特性默认值 */
    p->chara[C808_HAT_OPEN] = 1.0f;
    p->chara[C808_CLAP] = 0.65f;
    p->decay[C808_CRASH] = 0.72f;
    p->decay[C808_RIDE] = 0.45f;
    p->decay[C808_KICK] = 0.55f;
    p->level[C808_KICK] = 0.95f;
    p->level[C808_CRASH] = 0.6f;

    {
        static const uint8_t notes[C808_TRACKS] = {
            36, 38, 39, 37, 41, 45, 48, 64, 56, 70, 54, 49, 51, 46, 42, 75
        };
        for (t = 0; t < C808_TRACKS; ++t) p->midiNote[t] = notes[t];
    }

    p->master[C808_M_VOLUME] = 0.8f;
    p->master[C808_M_DRIVE]  = 0.15f;
    p->master[C808_M_HP]     = 0.05f;
    p->master[C808_M_LP]     = 0.85f;
    p->master[C808_M_RESO]   = 0.15f;
    p->master[C808_M_ACCENT] = 0.6f;
    p->clockSource = 0;
}

/* ========================================================================= */
/* 引擎生命周期                                                               */
/* ========================================================================= */
c808_engine* c808_create(double sample_rate)
{
    c808_engine* e;
    int t, v, i;
    if (sample_rate < 8000.0 || sample_rate > 384000.0) sample_rate = C808_SR_DEFAULT;

    e = (c808_engine*)calloc(1, sizeof(c808_engine));
    if (!e) return NULL;

    e->sr = (float)sample_rate;
    math_init(&e->math);
    c808_init_default_project(&e->projects[0]);
    c808_init_default_project(&e->projects[1]);
    atomic_init(&e->slot, 0);
    e->proj = &e->projects[0];
    atomic_init(&e->ring_head, 0);
    atomic_init(&e->ring_tail, 0);
    atomic_init(&e->midi_head, 0);
    atomic_init(&e->midi_tail, 0);
    e->playing = 0;
    e->samplePos = 0;
    e->nextGrid = 0;
    e->gridFrac = 0.0;
    e->stepSamples = (60.0 / 120.0 / 4.0) * e->sr;
    e->globalStep = 0;
    e->curPattern = 0;
    e->pendingPattern = -1;
    e->loopCount = 0;
    e->songPos = 0;
    e->songRep = 4;
    e->rng = 0x80808080u;
    e->midiClockCount = 0;

    for (t = 0; t < C808_TRACKS; ++t) {
        track_runtime_init(&e->tracks[t], t);
        for (v = 0; v < C808_VOICES_PER_TRACK; ++v) {
            memset(&e->voices[t][v], 0, sizeof(c808_voice_rt));
            e->voices[t][v].rng = 0x9E3779B9u ^ (uint32_t)(t * 131 + v * 17 + 1);
        }
        e->meter[t] = 0.0f;
    }
    for (i = 0; i < C808_M_COUNT; ++i) {
        e->masterSm[i] = e->proj->master[i];
        e->masterTarget[i] = e->masterSm[i];
    }
    return e;
}

void c808_destroy(c808_engine* e)
{
    if (!e) return;
    c808_midi_stop(e);
    free(e);
}

/* ========================================================================= */
/* 声部触发                                                                   */
/* ========================================================================= */
static int track_audible(const c808_engine* e, int track)
{
    const c808_project* p = e->proj;
    if (p->mute[track]) return 0;
    if (e->anySolo && !p->solo[track]) return 0;
    return 1;
}

static void voice_start(c808_engine* e, int track, float vel, float accent)
{
    const c808_project* p = e->proj;
    c808_track_rt* rt = &e->tracks[track];
    c808_voice_rt* v = NULL;
    int i, chosen = -1;
    uint64_t oldest = UINT64_MAX;
    float tune = rt->p[C808_P_TUNE];
    float decay = rt->p[C808_P_DECAY];
    float tone = rt->p[C808_P_TONE];
    float ch = rt->p[C808_P_CHAR];
    float sr = e->sr;

    for (i = 0; i < C808_VOICES_PER_TRACK; ++i) {
        if (!e->voices[track][i].active) { chosen = i; break; }
        if (e->voices[track][i].order < oldest) {
            oldest = e->voices[track][i].order;
            chosen = i;
        }
    }
    if (chosen < 0) chosen = 0;
    v = &e->voices[track][chosen];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->track = track;
    v->type = track;
    v->order = ++e->voiceOrder;
    v->vel = clampf(vel, 0.0f, 1.5f);
    v->accent = clampf(accent, 0.0f, 1.5f);
    v->choke = 0.0f;
    v->env = 1.0f;   /* 主包络从 1 开始指数衰减 */
    v->env2 = 1.0f;  /* 辅助包络（点击/噪声/尾音） */
    v->env3 = 0.0f;

    /* 每类声部的物理参数映射 ------------------------------------------------ */
    switch (track) {
    case C808_KICK: {
        float base = lerpf(34.0f, 88.0f, tune);
        v->f0 = base;
        v->pitchEnv = 1.0f;
        v->pitchCoef = decay_coef(lerpf(0.012f, 0.045f, tone), sr);
        v->coef1 = decay_coef(lerpf(0.10f, 2.4f, decay * decay), sr);
        v->coef2 = decay_coef(0.004f, sr);           /* click */
        v->phase[0] = 0.0f;
        v->aux = lerpf(0.15f, 0.65f, tone);          /* click 电平 */
        break;
    }
    case C808_SNARE: {
        v->f0 = lerpf(150.0f, 240.0f, tune);
        v->phase[1] = 0.0f;
        v->coef1 = decay_coef(lerpf(0.05f, 0.22f, decay), sr);
        v->coef2 = decay_coef(lerpf(0.06f, 0.35f, ch), sr);
        v->pitchEnv = 1.0f;
        v->pitchCoef = decay_coef(0.02f, sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(1200.0f, 4200.0f, tone), 200.0f, sr * 0.45f) / sr);
        v->k1 = 0.9f;
        break;
    }
    case C808_CLAP: {
        v->coef1 = decay_coef(lerpf(0.09f, 0.45f, decay), sr); /* 尾音 */
        v->coef2 = decay_coef(0.0035f, sr);                    /* 爆发 */
        v->aux = lerpf(0.006f, 0.014f, tone);                  /* 爆发间隔 */
        v->env3 = (ch > 0.75f) ? 4.0f : 3.0f;                  /* 爆发次数 */
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(800.0f, 2200.0f, tone), 200.0f, sr * 0.45f) / sr);
        v->k1 = 1.1f;
        break;
    }
    case C808_RIM: {
        v->f0 = lerpf(1500.0f, 2100.0f, tune);
        v->coef1 = decay_coef(lerpf(0.012f, 0.05f, decay), sr);
        v->coef2 = decay_coef(0.002f, sr);
        v->aux = 0.5f + tone * 0.4f;
        v->g1 = fast_tan(C808_M_PI * 2200.0f / sr);
        v->k1 = 0.8f;
        break;
    }
    case C808_TOM_LO:
    case C808_TOM_MID:
    case C808_TOM_HI: {
        float base = (track == C808_TOM_LO) ? 82.0f : (track == C808_TOM_MID) ? 120.0f : 168.0f;
        base = lerpf(base * 0.78f, base * 1.28f, tune);
        v->f0 = base;
        v->pitchEnv = 1.0f;
        v->pitchCoef = decay_coef(0.04f + ch * 0.03f, sr);
        v->coef1 = decay_coef(lerpf(0.12f, 1.15f, decay), sr);
        v->coef2 = decay_coef(0.006f, sr);
        v->aux = 0.2f + ch * 0.3f;
        break;
    }
    case C808_CONGA: {
        v->f0 = lerpf(190.0f, 320.0f, tune);
        v->pitchEnv = 1.0f;
        v->pitchCoef = decay_coef(0.025f, sr);
        v->coef1 = decay_coef(lerpf(0.12f, 0.7f, decay), sr);
        v->coef2 = decay_coef(0.004f, sr);
        v->aux = 0.15f + ch * 0.35f;
        break;
    }
    case C808_COWBELL: {
        v->f0 = lerpf(480.0f, 720.0f, tune);
        v->coef1 = decay_coef(lerpf(0.08f, 0.55f, decay), sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(1200.0f, 2600.0f, tone), 300.0f, sr * 0.45f) / sr);
        v->k1 = 1.4f;
        v->phase[0] = 0.0f; v->phase[1] = 0.0f;
        break;
    }
    case C808_SHAKER: {
        v->coef1 = decay_coef(lerpf(0.035f, 0.18f, decay), sr);
        v->coef2 = decay_coef(0.012f, sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(3500.0f, 9000.0f, tone), 500.0f, sr * 0.45f) / sr);
        v->k1 = 0.7f;
        break;
    }
    case C808_TAMB: {
        v->coef1 = decay_coef(lerpf(0.08f, 0.4f, decay), sr);
        v->coef2 = decay_coef(0.01f, sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(5000.0f, 10000.0f, tone), 500.0f, sr * 0.45f) / sr);
        v->k1 = 0.9f;
        break;
    }
    case C808_CRASH:
    case C808_RIDE: {
        int ride = (track == C808_RIDE);
        v->coef1 = decay_coef(ride ? lerpf(0.35f, 1.7f, decay) : lerpf(0.7f, 3.6f, decay), sr);
        v->coef2 = decay_coef(ride ? 0.15f : 0.3f, sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(3000.0f, 7000.0f, tone), 500.0f, sr * 0.45f) / sr);
        v->k1 = 1.0f;
        v->aux = ride ? 1.0f : 0.0f;
        v->f0 = ride ? lerpf(2100.0f, 3200.0f, tune) : 0.0f;
        break;
    }
    case C808_HAT_OPEN:
    case C808_HAT_CLOSED: {
        int open = (track == C808_HAT_OPEN);
        v->coef1 = decay_coef(open ? lerpf(0.18f, 1.4f, decay) : lerpf(0.025f, 0.10f, decay), sr);
        v->coef2 = decay_coef(0.004f, sr);
        v->g1 = fast_tan(C808_M_PI * clampf(lerpf(5500.0f, 11000.0f, tone), 500.0f, sr * 0.45f) / sr);
        v->k1 = 1.0f;
        v->aux = open ? 1.0f : 0.0f;
        break;
    }
    case C808_CLAVE: {
        v->f0 = lerpf(2200.0f, 2800.0f, tune);
        v->coef1 = decay_coef(lerpf(0.02f, 0.12f, decay), sr);
        v->coef2 = decay_coef(0.002f, sr);
        break;
    }
    default: break;
    }

    /* 开/闭镲 choke：闭镲触发时根据开镲 Mute Trigger 参数切断开镲 */
    if (track == C808_HAT_CLOSED && p->chara[C808_HAT_OPEN] >= 0.5f) {
        for (i = 0; i < C808_VOICES_PER_TRACK; ++i) {
            c808_voice_rt* ov = &e->voices[C808_HAT_OPEN][i];
            if (ov->active) {
                ov->choke = 1.0f;
                ov->coef1 *= 0.9995f;  /* 2ms 内快速衰减，避免爆音 */
            }
        }
    }
    e->triggerCount++;
}

/* ========================================================================= */
/* 单声部逐采样合成                                                           */
/* ========================================================================= */
static inline float voice_render(c808_engine* e, c808_voice_rt* v, const float* pr)
{
    float sr = e->sr;
    float tone = pr[C808_P_TONE];
    float ch = pr[C808_P_CHAR];
    float out = 0.0f;
    float accent = v->accent;
    int i;

    switch (v->type) {
    case C808_KICK: {
        float f = v->f0 * (1.0f + 3.6f * v->pitchEnv * (0.35f + 0.65f * tone)) + accent * 12.0f;
        float s, click;
        v->phase[0] += f / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        s = fast_sin(&e->math, v->phase[0]);
        click = voice_noise(&v->rng) * v->env2 * v->aux;
        out = s * v->env + click + 0.18f * fast_tanh(s * 3.0f) * v->env2;
        out = fast_tanh(out * (1.0f + accent * 1.1f)) * 0.85f;
        v->env2 *= v->coef2;
        v->pitchEnv *= v->pitchCoef;
        break;
    }
    case C808_SNARE: {
        float f1 = v->f0 * (1.0f + 0.9f * v->pitchEnv);
        float f2 = f1 * 1.83f;
        float osc, noise, bp;
        v->phase[0] += f1 / sr;
        v->phase[1] += f2 / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        if (v->phase[1] >= 1.0f) v->phase[1] -= 1.0f;
        osc = fast_sin(&e->math, v->phase[0]) + 0.55f * fast_sin(&e->math, v->phase[1]);
        noise = voice_noise(&v->rng);
        bp = svf_process(&v->f1, noise, v->g1, v->k1, 1);
        out = osc * 0.42f * v->env + bp * v->env2 * (0.35f + ch * 0.9f);
        out = fast_tanh(out * 1.4f) * 0.9f;
        v->pitchEnv *= v->pitchCoef;
        break;
    }
    case C808_CLAP: {
        float noise = voice_noise(&v->rng);
        float bp = svf_process(&v->f1, noise, v->g1, v->k1, 1);
        float burst = 0.0f;
        float age = v->aux > 0.0f ? (1.0f - v->env3) : 0.0f;
        (void)age;
        /* 用 env3 作为剩余爆发计数递减器：每到一个间隔重新点亮 env2 */
        v->env3 -= 1.0f / (v->aux * sr);
        if (v->env3 > 0.0f) {
            float frac = v->env3 - (float)(int)v->env3;
            burst = (frac > 0.72f) ? 1.0f : 0.0f;
        }
        out = bp * (v->env * 0.55f + burst * v->env2 * 1.4f);
        out *= (0.9f + ch * 0.4f);
        out = fast_tanh(out * 1.3f) * 0.9f;
        v->env2 *= v->coef2;
        break;
    }
    case C808_RIM: {
        float f = v->f0 * (1.0f + 0.4f * v->env2);
        float s, n;
        v->phase[0] += f / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        s = fast_sin(&e->math, v->phase[0]);
        n = svf_process(&v->f1, voice_noise(&v->rng), v->g1, v->k1, 1);
        out = (s * 0.8f + n * v->aux) * v->env;
        out += n * v->env2 * 0.5f;
        v->env2 *= v->coef2;
        break;
    }
    case C808_TOM_LO:
    case C808_TOM_MID:
    case C808_TOM_HI:
    case C808_CONGA: {
        float f = v->f0 * (1.0f + 0.85f * v->pitchEnv);
        float s, n;
        v->phase[0] += f / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        s = fast_sin(&e->math, v->phase[0]);
        n = voice_noise(&v->rng) * v->env2 * v->aux;
        out = (s * 0.85f + n) * v->env;
        out = fast_tanh(out * 1.2f) * 0.9f;
        v->pitchEnv *= v->pitchCoef;
        v->env2 *= v->coef2;
        break;
    }
    case C808_COWBELL: {
        float f1 = v->f0;
        float f2 = v->f0 * 1.48f;
        float a, b;
        v->phase[0] += f1 / sr;
        v->phase[1] += f2 / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        if (v->phase[1] >= 1.0f) v->phase[1] -= 1.0f;
        a = (v->phase[0] < 0.5f) ? 0.5f : -0.5f;
        b = (v->phase[1] < 0.5f) ? 0.5f : -0.5f;
        out = svf_process(&v->f1, a + b, v->g1, v->k1, 1) * v->env;
        out *= 1.2f;
        break;
    }
    case C808_SHAKER: {
        float n = voice_noise(&v->rng);
        float hp = svf_process(&v->f1, n, v->g1, v->k1, 2);
        float attack = 1.0f - v->env2;   /* env2 从 1 衰减 -> 形成快速起音 */
        out = hp * v->env * (0.25f + 0.75f * attack);
        v->env2 *= v->coef2;
        break;
    }
    case C808_TAMB: {
        float n = voice_noise(&v->rng);
        float hp = svf_process(&v->f1, n, v->g1, v->k1, 2);
        float metal = 0.0f;
        for (i = 0; i < 4; ++i) {
            float f = (float)(520 + i * 317) * (0.8f + tone * 0.6f);
            v->phase[i] += f / sr;
            if (v->phase[i] >= 1.0f) v->phase[i] -= 1.0f;
            metal += (v->phase[i] < 0.5f) ? 0.18f : -0.18f;
        }
        out = (hp * 0.8f + metal * v->env2) * v->env;
        v->env2 *= v->coef2;
        break;
    }
    case C808_CRASH:
    case C808_RIDE: {
        static const float ratio[6] = { 1.0f, 1.342f, 1.618f, 2.073f, 2.531f, 3.117f };
        float metal = 0.0f;
        float n = voice_noise(&v->rng);
        float hp;
        for (i = 0; i < 6; ++i) {
            float f = 317.0f * ratio[i] * (0.75f + tone * 0.6f);
            v->phase[i] += f / sr;
            if (v->phase[i] >= 1.0f) v->phase[i] -= 1.0f;
            metal += (v->phase[i] < 0.5f) ? 0.12f : -0.12f;
        }
        hp = svf_process(&v->f1, n * 0.7f + metal, v->g1, v->k1, 2);
        out = hp * v->env;
        if (v->aux > 0.5f) { /* Ride ping */
            v->phase[0] += v->f0 / sr;
            if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
            out += fast_sin(&e->math, v->phase[0]) * v->env2 * 0.35f;
            v->env2 *= v->coef2;
        }
        break;
    }
    case C808_HAT_OPEN:
    case C808_HAT_CLOSED: {
        static const float ratio[6] = { 1.0f, 1.483f, 1.932f, 2.546f, 3.117f, 4.104f };
        float metal = 0.0f;
        float bpv, hpv;
        for (i = 0; i < 6; ++i) {
            float f = 263.0f * ratio[i] * (0.8f + tone * 0.7f);
            v->phase[i] += f / sr;
            if (v->phase[i] >= 1.0f) v->phase[i] -= 1.0f;
            metal += (v->phase[i] < 0.5f) ? 0.16f : -0.16f;
        }
        hpv = svf_process(&v->f1, metal, v->g1, v->k1, 2);
        bpv = svf_process(&v->f2, hpv, v->g1 * 0.7f, 1.1f, 1);
        out = (hpv * 0.8f + bpv * 0.6f) * v->env;
        break;
    }
    case C808_CLAVE: {
        float f = v->f0;
        float s;
        v->phase[0] += f / sr;
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
        s = fast_sin(&e->math, v->phase[0]);
        out = (s * v->env + voice_noise(&v->rng) * v->env2 * 0.2f) * 0.8f;
        v->env2 *= v->coef2;
        break;
    }
    default:
        break;
    }

    if (v->choke > 0.0f) {
        v->env *= 0.9993f;
        if (v->env < 0.001f) v->active = 0;
    }
    /* 力度与重音总增益：重音同时参与音色（各声部内部）与整体响度 */
    out *= v->vel * (1.0f + v->accent * 0.35f);
    return out;
}

/* ========================================================================= */
/* 主效果：HP/LP + 饱和 + 软限幅                                              */
/* ========================================================================= */
static inline float master_process(c808_engine* e, float x, int channel)
{
    float drive = e->masterSm[C808_M_DRIVE];
    float pre = 1.0f + drive * 9.0f;
    float comp = 1.0f / (1.0f + drive * 2.6f);
    float y;
    int ch = channel & 1;
    y = svf_process(&e->hp[ch], x, e->hpG, e->hpK, 2);
    y = svf_process(&e->lp[ch], y, e->lpG, e->lpK, 0);
    y = fast_tanh(y * pre) * comp;
    /* DC blocker，防止饱和产生直流（左右声道独立） */
    {
        float dcy = y - e->dcx1[ch] + 0.995f * e->dcy1[ch];
        e->dcx1[ch] = y;
        e->dcy1[ch] = dcy;
        y = dcy;
    }
    y *= e->masterSm[C808_M_VOLUME] * 1.15f;
    if (y > 0.98f) y = 0.98f + (y - 0.98f) * 0.12f;
    if (y < -0.98f) y = -0.98f + (y + 0.98f) * 0.12f;
    return y;
}

/* ========================================================================= */
/* 音序器                                                                     */
/* ========================================================================= */
static void restart_song(c808_engine* e)
{
    const c808_project* p = e->proj;
    e->songPos = 0;
    if (p->songLen > 0) {
        e->songRep = p->song[0].reps ? p->song[0].reps : 1;
        e->curPattern = clampi(p->song[0].pat, 0, C808_PATTERNS - 1);
    } else {
        e->songRep = 1;
    }
}

static void apply_pattern_boundary(c808_engine* e)
{
    c808_project* p = &e->projects[atomic_load(&e->slot)];
    int len = clampi(p->patterns[e->curPattern].len, 1, C808_STEPS);

    e->loopCount++;
    if (e->pendingPattern >= 0) {
        e->curPattern = clampi(e->pendingPattern, 0, C808_PATTERNS - 1);
        e->pendingPattern = -1;
        if (p->songOn) restart_song(e);
        return;
    }
    if (p->songOn) {
        e->songRep--;
        if (e->songRep <= 0) {
            e->songPos = (e->songPos + 1) % (p->songLen > 0 ? p->songLen : 1);
            e->songRep = p->song[e->songPos].reps ? p->song[e->songPos].reps : 1;
            e->curPattern = clampi(p->song[e->songPos].pat, 0, C808_PATTERNS - 1);
        }
    } else {
        p->currentPattern = clampi(p->currentPattern, 0, C808_PATTERNS - 1);
        e->curPattern = p->currentPattern;
    }
    (void)len;
}

static inline int step_condition_ok(c808_engine* e, int cond, int condN)
{
    int loop = e->loopCount;
    switch (cond) {
    case C808_COND_1_2: return (loop & 1) == 0;
    case C808_COND_1_3: return (loop % 3) == 0;
    case C808_COND_1_4: return (loop % 4) == 0;
    case C808_COND_FILL: return (loop % 4) == 3;
    case C808_COND_NOTFILL: return (loop % 4) != 3;
    case C808_COND_EVERY_N: return condN > 0 && (loop % condN) == 0;
    default: return 1;
    }
}

static inline float rnd01(c808_engine* e)
{
    uint32_t x = e->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    e->rng = x;
    return (float)(x & 0xFFFFFF) * (1.0f / 16777216.0f);
}

static void do_step(c808_engine* e)
{
    const c808_project* p = e->proj;
    const c808_pattern* pat = &p->patterns[e->curPattern];
    int len = clampi(pat->len, 1, C808_STEPS);
    int step = e->globalStep;
    int t;
    double stepSamples = 60.0 / (double)clampf(p->bpm, 20.0f, 400.0f) / 4.0 * (double)e->sr;
    double swingOff = 0.0;
    float accentAmt = p->master[C808_M_ACCENT];

    if (stepSamples < 8.0) stepSamples = 8.0;
    e->stepSamples = stepSamples;
    if (pat->swing > 0 && (step & 1)) {
        swingOff = stepSamples * 0.5 * (double)pat->swing / 100.0;
    }

    if (step == 0 && e->loopCount > 0) {
        /* pattern/song 已在边界处理，这里无需重复 */
    }

    for (t = 0; t < C808_TRACKS; ++t) {
        c808_track_rt* rt = &e->tracks[t];
        int tlen = clampi(p->trackLen[t], 1, len);
        int s = step % tlen;
        const c808_pattern* q = pat;
        float vel;
        int micro;
        double start;
        int n, k;
        int accent;
        int cond, condN;

        rt->pendCount = 0;
        rt->pendIdx = 0;

        if (!q->on[t][s]) continue;
        cond = q->cond[t][s];
        condN = q->condN[t][s];
        if (!step_condition_ok(e, cond, condN)) continue;
        if (q->prob[t][s] < 100) {
            float r = rnd01(e);
            if (r * 100.0f >= (float)q->prob[t][s]) continue;
        }

        vel = (float)q->vel[t][s] * (1.0f / 127.0f);
        accent = q->accent[t][s] ? 1 : 0;
        if (accent) vel = clampf(vel * (1.0f + accentAmt * 0.65f), 0.0f, 1.5f);
        if (!track_audible(e, t)) {
            /* 静音/未 solo：不触发 voice，符合“音频路径中阻断触发” */
            continue;
        }

        micro = (int)q->micro[t][s];
        start = (double)e->nextGrid + swingOff + (double)micro;
        if (start < (double)e->samplePos) start = (double)e->samplePos;

        n = clampi(q->ratchet[t][s], 1, 8);
        {
            float rate = clampf((float)q->rate[t][s] * 0.01f, 0.05f, 1.0f);
            double spacing = (double)stepSamples * rate / (double)n;
            for (k = 0; k < n; ++k) {
                double tt = start + spacing * (double)k;
                uint64_t evTime;
                if (tt >= (double)e->nextGrid + stepSamples) break;
                evTime = (uint64_t)(tt + 0.5);
                rt->evTime[k] = evTime;
                rt->evVel[k] = vel * (k == 0 ? 1.0f : (1.0f - 0.12f * (float)k));
                rt->evAccent[k] = (uint8_t)(accent && k == 0);
                rt->pendCount++;
            }
        }
    }

    /* 推进网格：整数采样 + 小数余数，避免长时间累积漂移 */
    e->gridFrac += stepSamples;
    {
        uint64_t add = (uint64_t)e->gridFrac;
        e->gridFrac -= (double)add;
        e->nextGrid += add;
    }
    e->globalStep++;
    if (e->globalStep >= len) {
        e->globalStep = 0;
        apply_pattern_boundary(e);
    }
    e->uiStep = e->globalStep;
    e->uiPattern = e->curPattern;
    e->uiSongPos = e->songPos;
    e->uiLoop = e->loopCount;
}

/* MIDI 时钟驱动：每 6 个 clock 推进一步 */
static void midi_step(c808_engine* e)
{
    const c808_project* p = e->proj;
    const c808_pattern* pat = &p->patterns[e->curPattern];
    int len = clampi(pat->len, 1, C808_STEPS);
    e->nextGrid = (uint64_t)e->samplePos;
    do_step(e);
    (void)len;
}

/* ========================================================================= */
/* MIDI 事件（队列由 winmm/Android/Web 前端推送）                             */
/* ========================================================================= */
enum { C808_MIDI_NOTE = 1, C808_MIDI_CC, C808_MIDI_PROGRAM,
       C808_MIDI_CLOCK, C808_MIDI_START, C808_MIDI_STOP, C808_MIDI_CONTINUE };

int c808_push_midi(c808_engine* e, int type, int a, int b, int c)
{
    unsigned tail = atomic_load_explicit(&e->midi_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&e->midi_head, memory_order_acquire);
    if ((tail - head) >= 1024) return -1;
    e->midiRing[tail & 1023].type = type;
    e->midiRing[tail & 1023].a = a;
    e->midiRing[tail & 1023].b = b;
    e->midiRing[tail & 1023].c = c;
    atomic_store_explicit(&e->midi_tail, tail + 1, memory_order_release);
    return 0;
}

static void drain_midi(c808_engine* e)
{
    unsigned head = atomic_load_explicit(&e->midi_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&e->midi_tail, memory_order_acquire);
    while (head != tail) {
        int type = e->midiRing[head & 1023].type;
        int a = e->midiRing[head & 1023].a;
        int b = e->midiRing[head & 1023].b;
        int c = e->midiRing[head & 1023].c;
        head++;
        atomic_store_explicit(&e->midi_head, head, memory_order_release);

        switch (type) {
        case C808_MIDI_NOTE: {
            int t;
            for (t = 0; t < C808_TRACKS; ++t) {
                if (e->proj->midiNote[t] == (uint8_t)a) {
                    voice_start(e, t, (float)b / 127.0f, c);
                    break;
                }
            }
            break;
        }
        case C808_MIDI_PROGRAM:
            e->pendingPattern = clampi(a, 0, C808_PATTERNS - 1);
            if (!e->playing) {
                e->curPattern = e->pendingPattern;
                e->pendingPattern = -1;
            }
            break;
        case C808_MIDI_CLOCK:
            if (e->proj->clockSource == 1 && e->playing) {
                e->midiClockCount++;
                if (e->midiClockCount >= 6) {
                    e->midiClockCount = 0;
                    midi_step(e);
                }
            }
            break;
        case C808_MIDI_START:
            e->playing = 1;
            e->globalStep = 0;
            e->loopCount = 0;
            e->midiClockCount = 0;
            e->nextGrid = (uint64_t)e->samplePos;
            if (e->proj->songOn) restart_song(e);
            else e->curPattern = e->pendingPattern >= 0 ? e->pendingPattern : e->proj->currentPattern;
            e->pendingPattern = -1;
            break;
        case C808_MIDI_STOP:
            e->playing = 0;
            break;
        case C808_MIDI_CONTINUE:
            e->playing = 1;
            e->nextGrid = (uint64_t)e->samplePos;
            break;
        default:
            break;
        }
    }
}

/* ========================================================================= */
/* 命令应用（音频线程块首）                                                   */
/* ========================================================================= */
static void cmd_apply(c808_engine* e, const c808_cmd* c)
{
    int slot = atomic_load(&e->slot);
    c808_project* p = &e->projects[slot];
    switch (c->type) {
    case C808_CMD_SET_STEP_ON:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].on[c->b][c->c] = (uint8_t)(c->f > 0.5f);
        break;
    case C808_CMD_SET_STEP_VEL:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].vel[c->b][c->c] = (uint8_t)clampi((int)c->f, 1, 127);
        break;
    case C808_CMD_SET_STEP_ACCENT:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].accent[c->b][c->c] = (uint8_t)(c->f > 0.5f);
        break;
    case C808_CMD_SET_STEP_PROB:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].prob[c->b][c->c] = (uint8_t)clampi((int)c->f, 0, 100);
        break;
    case C808_CMD_SET_STEP_RATCHET:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].ratchet[c->b][c->c] = (uint8_t)clampi((int)c->f, 1, 8);
        break;
    case C808_CMD_SET_STEP_RATE:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].rate[c->b][c->c] = (uint8_t)clampi((int)c->f, 10, 100);
        break;
    case C808_CMD_SET_STEP_COND:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].cond[c->b][c->c] = (uint8_t)clampi((int)c->f, 0, C808_COND_EVERY_N);
        break;
    case C808_CMD_SET_STEP_CONDN:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].condN[c->b][c->c] = (uint8_t)clampi((int)c->f, 1, 8);
        break;
    case C808_CMD_SET_STEP_MICRO:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS &&
            c->c >= 0 && c->c < C808_STEPS)
            p->patterns[c->a].micro[c->b][c->c] = (int16_t)clampi((int)c->f, -2400, 2400);
        break;
    case C808_CMD_SET_TRACK_PARAM:
        if (c->a >= 0 && c->a < C808_TRACKS && c->b >= 0 && c->b < C808_P_COUNT) {
            float v = (c->b == C808_P_PAN) ? f01(c->f) : f01(c->f);
            p->tune[c->a]  = (c->b == C808_P_TUNE) ? v : p->tune[c->a];
            p->decay[c->a] = (c->b == C808_P_DECAY) ? v : p->decay[c->a];
            p->tone[c->a]  = (c->b == C808_P_TONE) ? v : p->tone[c->a];
            p->chara[c->a] = (c->b == C808_P_CHAR) ? v : p->chara[c->a];
            p->level[c->a] = (c->b == C808_P_LEVEL) ? v : p->level[c->a];
            p->pan[c->a]   = (c->b == C808_P_PAN) ? v : p->pan[c->a];
            e->tracks[c->a].target[c->b] = v;
        }
        break;
    case C808_CMD_SET_TRACK_LEN:
        if (c->a >= 0 && c->a < C808_TRACKS)
            p->trackLen[c->a] = (uint8_t)clampi(c->b, 1, C808_STEPS);
        break;
    case C808_CMD_SET_TRACK_MUTE:
        if (c->a >= 0 && c->a < C808_TRACKS) {
            p->mute[c->a] = (uint8_t)(c->f > 0.5f);
            if (p->mute[c->a]) {
                int i;
                for (i = 0; i < C808_VOICES_PER_TRACK; ++i) e->voices[c->a][i].active = 0;
            }
        }
        break;
    case C808_CMD_SET_TRACK_SOLO:
        if (c->a >= 0 && c->a < C808_TRACKS)
            p->solo[c->a] = (uint8_t)(c->f > 0.5f);
        break;
    case C808_CMD_SET_MASTER:
        if (c->b >= 0 && c->b < C808_M_COUNT) {
            p->master[c->b] = f01(c->f);
            e->masterTarget[c->b] = p->master[c->b];
        }
        break;
    case C808_CMD_SET_BPM:
        p->bpm = clampf(c->f, 20.0f, 400.0f);
        break;
    case C808_CMD_SET_TIMESIG:
        p->tsNum = clampi(c->a, 1, 16);
        p->tsDen = clampi(c->b, 1, 16);
        break;
    case C808_CMD_SET_CLOCK:
        p->clockSource = c->a ? 1 : 0;
        e->midiClockCount = 0;
        break;
    case C808_CMD_SET_PATTERN_LEN:
        if (c->a >= 0 && c->a < C808_PATTERNS)
            p->patterns[c->a].len = (uint8_t)clampi(c->b, 1, C808_STEPS);
        break;
    case C808_CMD_SET_PATTERN_SWING:
        if (c->a >= 0 && c->a < C808_PATTERNS)
            p->patterns[c->a].swing = (uint8_t)clampi(c->b, 0, 100);
        break;
    case C808_CMD_SELECT_PATTERN:
        p->currentPattern = clampi(c->a, 0, C808_PATTERNS - 1);
        if (e->playing) e->pendingPattern = p->currentPattern;
        else e->curPattern = p->currentPattern;
        break;
    case C808_CMD_SET_SONG_ON:
        p->songOn = c->a ? 1 : 0;
        if (p->songOn) restart_song(e);
        break;
    case C808_CMD_SET_SONG_LEN:
        p->songLen = clampi(c->a, 1, C808_SONG_ENTRIES);
        break;
    case C808_CMD_SET_SONG_ENTRY:
        if (c->a >= 0 && c->a < C808_SONG_ENTRIES) {
            p->song[c->a].pat = (uint16_t)clampi(c->b, 0, C808_PATTERNS - 1);
            p->song[c->a].reps = (uint16_t)clampi(c->c, 1, 64);
        }
        break;
    case C808_CMD_SET_PATTERN_COUNT:
        p->patternCount = clampi(c->a, 1, C808_PATTERNS);
        break;
    case C808_CMD_CLEAR_PATTERN:
        if (c->a >= 0 && c->a < C808_PATTERNS) {
            int t, s;
            for (t = 0; t < C808_TRACKS; ++t)
                for (s = 0; s < C808_STEPS; ++s)
                    p->patterns[c->a].on[t][s] = 0;
        }
        break;
    case C808_CMD_CLEAR_TRACK:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_TRACKS) {
            int s;
            for (s = 0; s < C808_STEPS; ++s) p->patterns[c->a].on[c->b][s] = 0;
        }
        break;
    case C808_CMD_COPY_PATTERN:
        if (c->a >= 0 && c->a < C808_PATTERNS && c->b >= 0 && c->b < C808_PATTERNS)
            p->patterns[c->b] = p->patterns[c->a];
        break;
    case C808_CMD_TRANSPORT_PLAY:
        e->playing = 1;
        e->globalStep = 0;
        e->loopCount = 0;
        e->gridFrac = 0.0;
        e->nextGrid = (uint64_t)e->samplePos;
        e->midiClockCount = 0;
        e->curPattern = p->songOn ? e->curPattern : p->currentPattern;
        if (p->songOn) restart_song(e);
        break;
    case C808_CMD_TRANSPORT_STOP:
        e->playing = 0;
        break;
    case C808_CMD_TRANSPORT_STOP_RESET:
        e->playing = 0;
        e->globalStep = 0;
        e->loopCount = 0;
        e->uiStep = 0;
        break;
    case C808_CMD_PANIC: {
        int t, v;
        for (t = 0; t < C808_TRACKS; ++t)
            for (v = 0; v < C808_VOICES_PER_TRACK; ++v)
                e->voices[t][v].active = 0;
        break;
    }
    case C808_CMD_NOTE_ON:
        if (c->a >= 0 && c->a < C808_TRACKS)
            voice_start(e, c->a, (float)c->b / 127.0f, (float)c->c);
        break;
    case C808_CMD_SET_TRACK_NOTE:
        if (c->a >= 0 && c->a < C808_TRACKS)
            p->midiNote[c->a] = (uint8_t)clampi(c->b, 0, 127);
        break;
    case C808_CMD_ACTIVATE_SLOT: {
        int newSlot = c->a & 1;
        atomic_store(&e->slot, newSlot);
        e->proj = &e->projects[newSlot];
        e->curPattern = clampi(e->proj->currentPattern, 0, C808_PATTERNS - 1);
        e->globalStep = 0;
        e->loopCount = 0;
        e->nextGrid = (uint64_t)e->samplePos;
        e->gridFrac = 0.0;
        if (e->proj->songOn) restart_song(e);
        {
            int t, i;
            for (t = 0; t < C808_TRACKS; ++t) {
                c808_track_rt* rt = &e->tracks[t];
                for (i = 0; i < C808_P_COUNT; ++i) {
                    rt->target[i] = (i == C808_P_TUNE) ? e->proj->tune[t] :
                                    (i == C808_P_DECAY) ? e->proj->decay[t] :
                                    (i == C808_P_TONE) ? e->proj->tone[t] :
                                    (i == C808_P_CHAR) ? e->proj->chara[t] :
                                    (i == C808_P_LEVEL) ? e->proj->level[t] :
                                    e->proj->pan[t];
                }
                rt->pendCount = 0;
                rt->pendIdx = 0;
            }
            for (i = 0; i < C808_M_COUNT; ++i)
                e->masterTarget[i] = e->proj->master[i];
        }
        break;
    }
    default:
        break;
    }
}

/* ========================================================================= */
/* 渲染主循环                                                                 */
/* ========================================================================= */
void c808_render(c808_engine* e, float* outL, float* outR, int frames)
{
    int i, t, v;
    int soloAny = 0;
    c808_cmd cmd;
    const c808_project* p;

    if (!e || !outL || !outR || frames <= 0) return;

    /* 1) 块首：应用 UI 命令、MIDI 事件，读取活动工程 */
    while (ring_pop(e, &cmd)) cmd_apply(e, &cmd);
    drain_midi(e);
    p = &e->projects[atomic_load(&e->slot)];
    e->proj = p;

    for (t = 0; t < C808_TRACKS; ++t) {
        if (p->solo[t]) { soloAny = 1; break; }
    }
    e->anySolo = soloAny;

    /* 2) 主参数块级平滑 + 滤波系数 */
    for (i = 0; i < C808_M_COUNT; ++i) {
        e->masterTarget[i] = p->master[i];
        e->masterSm[i] += (e->masterTarget[i] - e->masterSm[i]) * 0.35f;
    }
    {
        float hpHz = 20.0f + e->masterSm[C808_M_HP] * e->masterSm[C808_M_HP] * 2400.0f;
        float lpHz = 900.0f + e->masterSm[C808_M_LP] * e->masterSm[C808_M_LP] *
                     e->masterSm[C808_M_LP] * 20000.0f;
        float res = e->masterSm[C808_M_RESO];
        float q = 0.55f + res * 7.0f;
        e->hpG = fast_tan(C808_M_PI * clampf(hpHz, 15.0f, e->sr * 0.45f) / e->sr);
        e->hpK = 1.0f / q;
        e->lpG = fast_tan(C808_M_PI * clampf(lpHz, 200.0f, e->sr * 0.45f) / e->sr);
        e->lpK = 1.0f / q;
    }

    /* 3) 每轨参数平滑与声像增益 */
    for (t = 0; t < C808_TRACKS; ++t) {
        c808_track_rt* rt = &e->tracks[t];
        rt->target[C808_P_TUNE]  = p->tune[t];
        rt->target[C808_P_DECAY] = p->decay[t];
        rt->target[C808_P_TONE]  = p->tone[t];
        rt->target[C808_P_CHAR]  = p->chara[t];
        rt->target[C808_P_LEVEL] = p->level[t];
        rt->target[C808_P_PAN]   = p->pan[t];
        for (i = 0; i < C808_P_COUNT; ++i) {
            rt->p[i] += (rt->target[i] - rt->p[i]) * 0.25f;
        }
        {
            float pan = clampf(rt->p[C808_P_PAN] * 2.0f - 1.0f, -1.0f, 1.0f);
            /* 等功率声像：pan=-1 -> 0 圈(全左)，0 -> 1/8 圈(左右各 0.7071)，+1 -> 1/4 圈(全右)。
               注意 fast_cos/fast_sin 的相位单位是“圈”，不是弧度。 */
            float a = (pan + 1.0f) * 0.125f;     /* 0..0.25 圈 */
            rt->gainL = fast_cos(&e->math, a);
            rt->gainR = fast_sin(&e->math, a);
        }
        /* 更新活动 voice 的滤波系数（音色实时变化） */
        for (v = 0; v < C808_VOICES_PER_TRACK; ++v) {
            c808_voice_rt* vv = &e->voices[t][v];
            if (!vv->active) continue;
            if (t == C808_SNARE || t == C808_CLAP) {
                float f = (t == C808_SNARE) ? lerpf(1200.0f, 4200.0f, rt->p[C808_P_TONE])
                                             : lerpf(800.0f, 2200.0f, rt->p[C808_P_TONE]);
                vv->g1 = fast_tan(C808_M_PI * clampf(f, 200.0f, e->sr * 0.45f) / e->sr);
            }
        }
    }

    /* 4) 逐采样：音序、触发、合成、混音、主效果 */
    for (i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        int64_t now = e->samplePos;

        if (e->playing && p->clockSource == 0) {
            int guard = 0;
            while ((uint64_t)now >= e->nextGrid && guard++ < 2) {
                do_step(e);
            }
        }

        for (t = 0; t < C808_TRACKS; ++t) {
            c808_track_rt* rt = &e->tracks[t];
            float mono = 0.0f;
            while (rt->pendIdx < rt->pendCount &&
                   (uint64_t)now >= rt->evTime[rt->pendIdx]) {
                voice_start(e, t, rt->evVel[rt->pendIdx],
                            rt->evAccent[rt->pendIdx] ? 1.0f : 0.0f);
                rt->pendIdx++;
            }
            for (v = 0; v < C808_VOICES_PER_TRACK; ++v) {
                c808_voice_rt* vv = &e->voices[t][v];
                float s;
                if (!vv->active) continue;
                s = voice_render(e, vv, rt->p);
                mono += s;
                if (vv->env < 0.00006f) vv->active = 0;
            }
            mono *= rt->p[C808_P_LEVEL];
            if (mono > 1.6f) mono = 1.6f;
            if (mono < -1.6f) mono = -1.6f;
            {
                float a = mono < 0.0f ? -mono : mono;
                if (a > e->meter[t]) e->meter[t] = a;
            }
            if (!track_audible(e, t)) mono = 0.0f;
            l += mono * rt->gainL;
            r += mono * rt->gainR;
        }

        l = master_process(e, l, 0);
        r = master_process(e, r, 1);
        outL[i] = l;
        outR[i] = r;
        {
            float a = l < 0.0f ? -l : l;
            if (a > e->meterL) e->meterL = a;
        }
        {
            float a = r < 0.0f ? -r : r;
            if (a > e->meterR) e->meterR = a;
        }
        e->samplePos = now + 1;
    }

    /* 5) 电平表回落，供 UI 30fps 读取 */
    for (t = 0; t < C808_TRACKS; ++t) e->meter[t] *= 0.82f;
    e->meterL *= 0.82f;
    e->meterR *= 0.82f;
}

void c808_render_interleaved(c808_engine* e, float* out, int frames)
{
    float l[256], r[256];
    if (!e || !out || frames <= 0) return;
    while (frames > 0) {
        int n = frames > 256 ? 256 : frames;
        int i;
        c808_render(e, l, r, n);
        for (i = 0; i < n; ++i) {
            out[i * 2] = l[i];
            out[i * 2 + 1] = r[i];
        }
        out += n * 2;
        frames -= n;
    }
}

/* ========================================================================= */
/* 状态与命令封装                                                             */
/* ========================================================================= */
void c808_get_status(c808_engine* e, c808_status* out)
{
    int t;
    if (!e || !out) return;
    memset(out, 0, sizeof(*out));
    out->playing = e->playing;
    out->pattern = e->curPattern;
    out->step = e->globalStep;
    out->songPos = e->songPos;
    out->loopCount = e->loopCount;
    out->xruns = (int)e->xruns;
    out->cpuLoad = e->cpuLoad;
    out->masterL = e->meterL;
    out->masterR = e->meterR;
    for (t = 0; t < C808_TRACKS; ++t) out->meter[t] = e->meter[t];
}

void c808_report_xrun(c808_engine* e) { if (e) e->xruns++; }
void c808_report_cpu(c808_engine* e, float load) { if (e) e->cpuLoad = clampf(load, 0.0f, 1.0f); }

const c808_project* c808_project_ro(c808_engine* e)
{
    if (!e) return NULL;
    return &e->projects[atomic_load(&e->slot)];
}

c808_project* c808_project_shadow(c808_engine* e)
{
    int slot;
    if (!e) return NULL;
    slot = atomic_load(&e->slot);
    return &e->projects[slot ^ 1];
}

int c808_activate_shadow(c808_engine* e)
{
    int slot;
    if (!e) return -1;
    slot = atomic_load(&e->slot);
    return push0(e, C808_CMD_ACTIVATE_SLOT, slot ^ 1, 0, 0, 0.0f);
}

uint64_t c808_trigger_count(c808_engine* e) { return e ? e->triggerCount : 0; }

/* ------------------------------------------------------------------------- */
/* 跨语言只读访问                                                             */
/* ------------------------------------------------------------------------- */
int c808_get_step(c808_engine* e, int pat, int track, int step, int field)
{
    const c808_pattern* q;
    if (!e || pat < 0 || pat >= C808_PATTERNS || track < 0 || track >= C808_TRACKS ||
        step < 0 || step >= C808_STEPS) return 0;
    q = &e->projects[atomic_load(&e->slot)].patterns[pat];
    switch (field) {
    case C808_F_ON: return q->on[track][step];
    case C808_F_VEL: return q->vel[track][step];
    case C808_F_ACCENT: return q->accent[track][step];
    case C808_F_PROB: return q->prob[track][step];
    case C808_F_RATCHET: return q->ratchet[track][step];
    case C808_F_RATE: return q->rate[track][step];
    case C808_F_COND: return q->cond[track][step];
    case C808_F_CONDN: return q->condN[track][step];
    case C808_F_MICRO: return q->micro[track][step];
    default: return 0;
    }
}

float c808_get_track_param(c808_engine* e, int track, int param)
{
    const c808_project* p;
    if (!e || track < 0 || track >= C808_TRACKS) return 0.0f;
    p = &e->projects[atomic_load(&e->slot)];
    switch (param) {
    case C808_P_TUNE: return p->tune[track];
    case C808_P_DECAY: return p->decay[track];
    case C808_P_TONE: return p->tone[track];
    case C808_P_CHAR: return p->chara[track];
    case C808_P_LEVEL: return p->level[track];
    case C808_P_PAN: return p->pan[track];
    default: return 0.0f;
    }
}

int c808_get_track_len(c808_engine* e, int track)
{
    return (e && track >= 0 && track < C808_TRACKS) ?
        e->projects[atomic_load(&e->slot)].trackLen[track] : 0;
}
int c808_get_track_mute(c808_engine* e, int track)
{
    return (e && track >= 0 && track < C808_TRACKS) ?
        e->projects[atomic_load(&e->slot)].mute[track] : 0;
}
int c808_get_track_solo(c808_engine* e, int track)
{
    return (e && track >= 0 && track < C808_TRACKS) ?
        e->projects[atomic_load(&e->slot)].solo[track] : 0;
}
int c808_get_track_note(c808_engine* e, int track)
{
    return (e && track >= 0 && track < C808_TRACKS) ?
        e->projects[atomic_load(&e->slot)].midiNote[track] : 0;
}
float c808_get_master(c808_engine* e, int param)
{
    if (!e || param < 0 || param >= C808_M_COUNT) return 0.0f;
    return e->projects[atomic_load(&e->slot)].master[param];
}
float c808_get_bpm(c808_engine* e)
{
    return e ? e->projects[atomic_load(&e->slot)].bpm : 120.0f;
}
int c808_get_pattern_len(c808_engine* e, int pat)
{
    if (!e || pat < 0 || pat >= C808_PATTERNS) return 16;
    return e->projects[atomic_load(&e->slot)].patterns[pat].len;
}
int c808_get_pattern_swing(c808_engine* e, int pat)
{
    if (!e || pat < 0 || pat >= C808_PATTERNS) return 0;
    return e->projects[atomic_load(&e->slot)].patterns[pat].swing;
}
int c808_get_current_pattern(c808_engine* e)
{
    if (!e) return 0;
    return e->curPattern;
}
int c808_get_pattern_count(c808_engine* e)
{
    return e ? e->projects[atomic_load(&e->slot)].patternCount : 1;
}
int c808_get_song_on(c808_engine* e)
{
    return e ? e->projects[atomic_load(&e->slot)].songOn : 0;
}
int c808_get_song_len(c808_engine* e)
{
    return e ? e->projects[atomic_load(&e->slot)].songLen : 0;
}
int c808_get_song_entry(c808_engine* e, int index, int* pat, int* reps)
{
    const c808_project* p;
    if (!e || index < 0 || index >= C808_SONG_ENTRIES) return -1;
    p = &e->projects[atomic_load(&e->slot)];
    if (pat) *pat = p->song[index].pat;
    if (reps) *reps = p->song[index].reps;
    return 0;
}
int c808_get_timesig(c808_engine* e, int* num, int* den)
{
    const c808_project* p;
    if (!e) return -1;
    p = &e->projects[atomic_load(&e->slot)];
    if (num) *num = p->tsNum;
    if (den) *den = p->tsDen;
    return 0;
}
int c808_get_clock_source(c808_engine* e)
{
    return e ? e->projects[atomic_load(&e->slot)].clockSource : 0;
}

void c808_snapshot(c808_engine* e, int pat, int32_t* meta, float* params,
                   uint8_t* steps, uint8_t* flags)
{
    const c808_project* p;
    const c808_pattern* q;
    int t, s, i;
    if (!e) return;
    if (pat < 0 || pat >= C808_PATTERNS) pat = e->curPattern;
    p = &e->projects[atomic_load(&e->slot)];
    q = &p->patterns[pat];
    if (meta) {
        meta[0] = q->len;
        meta[1] = q->swing;
        meta[2] = e->curPattern;
        meta[3] = p->songOn;
        meta[4] = p->patternCount;
        meta[5] = p->tsNum;
        meta[6] = p->tsDen;
    }
    if (params) {
        params[0] = p->bpm;
        for (i = 0; i < C808_M_COUNT; ++i) params[1 + i] = p->master[i];
        for (t = 0; t < C808_TRACKS; ++t) {
            params[7 + t * 6 + 0] = p->tune[t];
            params[7 + t * 6 + 1] = p->decay[t];
            params[7 + t * 6 + 2] = p->tone[t];
            params[7 + t * 6 + 3] = p->chara[t];
            params[7 + t * 6 + 4] = p->level[t];
            params[7 + t * 6 + 5] = p->pan[t];
        }
    }
    if (steps) {
        uint8_t* o = steps;
        for (t = 0; t < C808_TRACKS; ++t) {
            for (s = 0; s < C808_STEPS; ++s) {
                int16_t micro = q->micro[t][s];
                o[0] = q->on[t][s];
                o[1] = q->vel[t][s];
                o[2] = q->accent[t][s];
                o[3] = q->prob[t][s];
                o[4] = q->ratchet[t][s];
                o[5] = q->rate[t][s];
                o[6] = q->cond[t][s];
                o[7] = q->condN[t][s];
                o[8] = (uint8_t)(micro & 0xff);
                o[9] = (uint8_t)((micro >> 8) & 0xff);
                o += 10;
            }
        }
    }
    if (flags) {
        for (t = 0; t < C808_TRACKS; ++t) {
            flags[t * 4 + 0] = p->mute[t];
            flags[t * 4 + 1] = p->solo[t];
            flags[t * 4 + 2] = p->trackLen[t];
            flags[t * 4 + 3] = p->midiNote[t];
        }
    }
}

/* 便捷封装 ------------------------------------------------------------------ */
#define CMD1(fn, t, A, B, C) \
    int fn(c808_engine* e, int A, int B, int C) { \
        return push0(e, t, A, B, C, 0.0f); \
    }
#define CMD2(fn, t, A, B, C, F) \
    int fn(c808_engine* e, int A, int B, int C) { \
        return push0(e, t, A, B, C, (float)F); \
    }

int c808_set_step_on(c808_engine* e, int pat, int track, int step, int on)
{ return push0(e, C808_CMD_SET_STEP_ON, pat, track, step, on ? 1.0f : 0.0f); }

int c808_set_step_vel(c808_engine* e, int pat, int track, int step, int vel)
{ return push0(e, C808_CMD_SET_STEP_VEL, pat, track, step, (float)vel); }

int c808_set_step_accent(c808_engine* e, int pat, int track, int step, int accent)
{ return push0(e, C808_CMD_SET_STEP_ACCENT, pat, track, step, accent ? 1.0f : 0.0f); }

int c808_set_step_prob(c808_engine* e, int pat, int track, int step, int prob)
{ return push0(e, C808_CMD_SET_STEP_PROB, pat, track, step, (float)prob); }

int c808_set_step_ratchet(c808_engine* e, int pat, int track, int step, int count)
{ return push0(e, C808_CMD_SET_STEP_RATCHET, pat, track, step, (float)count); }

int c808_set_step_rate(c808_engine* e, int pat, int track, int step, int rate)
{ return push0(e, C808_CMD_SET_STEP_RATE, pat, track, step, (float)rate); }

int c808_set_step_cond(c808_engine* e, int pat, int track, int step, int cond, int condN)
{
    int rc = push0(e, C808_CMD_SET_STEP_CONDN, pat, track, step, (float)condN);
    if (rc) return rc;
    return push0(e, C808_CMD_SET_STEP_COND, pat, track, step, (float)cond);
}

int c808_set_step_micro(c808_engine* e, int pat, int track, int step, int microSamples)
{ return push0(e, C808_CMD_SET_STEP_MICRO, pat, track, step, (float)microSamples); }

int c808_set_track_param(c808_engine* e, int track, int param, float value)
{ return push0(e, C808_CMD_SET_TRACK_PARAM, track, param, 0, value); }

int c808_set_track_len(c808_engine* e, int track, int len)
{ return push0(e, C808_CMD_SET_TRACK_LEN, track, len, 0, 0.0f); }

int c808_set_mute(c808_engine* e, int track, int mute)
{ return push0(e, C808_CMD_SET_TRACK_MUTE, track, 0, 0, mute ? 1.0f : 0.0f); }

int c808_set_solo(c808_engine* e, int track, int solo)
{ return push0(e, C808_CMD_SET_TRACK_SOLO, track, 0, 0, solo ? 1.0f : 0.0f); }

int c808_set_master(c808_engine* e, int param, float value)
{ return push0(e, C808_CMD_SET_MASTER, 0, param, 0, value); }

int c808_set_bpm(c808_engine* e, float bpm)
{ return push0(e, C808_CMD_SET_BPM, 0, 0, 0, bpm); }

int c808_set_timesig(c808_engine* e, int num, int den)
{ return push0(e, C808_CMD_SET_TIMESIG, num, den, 0, 0.0f); }

int c808_set_clock_source(c808_engine* e, int midiClock)
{ return push0(e, C808_CMD_SET_CLOCK, midiClock ? 1 : 0, 0, 0, 0.0f); }

int c808_set_pattern_len(c808_engine* e, int pat, int len)
{ return push0(e, C808_CMD_SET_PATTERN_LEN, pat, len, 0, 0.0f); }

int c808_set_pattern_swing(c808_engine* e, int pat, int swing)
{ return push0(e, C808_CMD_SET_PATTERN_SWING, pat, swing, 0, 0.0f); }

int c808_select_pattern(c808_engine* e, int pat)
{ return push0(e, C808_CMD_SELECT_PATTERN, pat, 0, 0, 0.0f); }

int c808_set_song_on(c808_engine* e, int on)
{ return push0(e, C808_CMD_SET_SONG_ON, on, 0, 0, 0.0f); }

int c808_set_song_entry(c808_engine* e, int index, int pat, int reps)
{ return push0(e, C808_CMD_SET_SONG_ENTRY, index, pat, reps, 0.0f); }

int c808_set_song_len(c808_engine* e, int len)
{ return push0(e, C808_CMD_SET_SONG_LEN, len, 0, 0, 0.0f); }

int c808_set_pattern_count(c808_engine* e, int count)
{ return push0(e, C808_CMD_SET_PATTERN_COUNT, count, 0, 0, 0.0f); }

int c808_clear_pattern(c808_engine* e, int pat)
{ return push0(e, C808_CMD_CLEAR_PATTERN, pat, 0, 0, 0.0f); }

int c808_clear_track(c808_engine* e, int pat, int track)
{ return push0(e, C808_CMD_CLEAR_TRACK, pat, track, 0, 0.0f); }

int c808_copy_pattern(c808_engine* e, int src, int dst)
{ return push0(e, C808_CMD_COPY_PATTERN, src, dst, 0, 0.0f); }

int c808_transport_play(c808_engine* e)
{ return push0(e, C808_CMD_TRANSPORT_PLAY, 0, 0, 0, 0.0f); }

int c808_transport_stop(c808_engine* e)
{ return push0(e, C808_CMD_TRANSPORT_STOP, 0, 0, 0, 0.0f); }

int c808_transport_stop_reset(c808_engine* e)
{ return push0(e, C808_CMD_TRANSPORT_STOP_RESET, 0, 0, 0, 0.0f); }

int c808_note_on(c808_engine* e, int track, int vel, int accent)
{ return push0(e, C808_CMD_NOTE_ON, track, vel, accent, 0.0f); }

int c808_panic(c808_engine* e)
{ return push0(e, C808_CMD_PANIC, 0, 0, 0, 0.0f); }

int c808_activate_slot(c808_engine* e, int slot)
{ return push0(e, C808_CMD_ACTIVATE_SLOT, slot & 1, 0, 0, 0.0f); }
