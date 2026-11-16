/* ==========================================================================
 *  engine.c —— 音序器、混音、主效果、waveOut 输出、导出（纯 C 版）
 *
 *  徐某的实时纪律，在这个文件里体现得最彻底：
 *    1. samplePos（long long）是唯一时钟；
 *    2. 所有事件先展开成绝对样本时刻，再按块切段渲染；
 *    3. 回调里不 malloc/free、不加锁、不读写文件；
 *    4. 停止时给 voice 10ms 淡出，既即时安静又不“咔哒”。
 * ========================================================================== */
#include "xu808.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>

/* 一些编译器不会自动链接 winmm，这里显式告诉链接器 */
#pragma comment(lib, "winmm.lib")

/* ---------------- 默认工程 ---------------- */
void xu_project_demo(XuPattern* pat)
{
    int p, s;
    memset(pat, 0, sizeof(*pat));
    /* 清空后每一格都是“关”，再逐格点亮演示律动 */
    for (p = 0; p < XU_PADS; p++)
        for (s = 0; s < XU_STEPS_MAX; s++)
            pat->data[p][s].velocity = 100,
                pat->data[p][s].prob = 100,
                pat->data[p][s].ratchet = 1;

    pat->steps = 16;
    strcpy(pat->name, "PATTERN 1");

    /* 底鼓：正拍与反拍 */
    pat->data[0][0].on = 1;
    pat->data[0][0].velocity = 118;
    pat->data[0][0].accent = 1;
    pat->data[0][8].on = 1;
    pat->data[0][8].velocity = 105;
    pat->data[0][14].on = 1;
    pat->data[0][14].velocity = 90;
    /* 军鼓：2、4 拍 */
    pat->data[1][4].on = 1;
    pat->data[1][4].velocity = 110;
    pat->data[1][12].on = 1;
    pat->data[1][12].velocity = 110;
    pat->data[1][12].accent = 1;
    /* 闭镲：八分音符 */
    for (s = 0; s < 16; s += 2)
    {
        pat->data[15][s].on = 1;
        pat->data[15][s].velocity = (s % 4 == 0) ? 96 : 84;
    }
    /* 开镲、拍手、沙锤 */
    pat->data[14][14].on = 1;
    pat->data[14][14].velocity = 92;
    pat->data[10][12].on = 1;
    pat->data[10][12].velocity = 80;
    for (s = 1; s < 16; s += 2)
    {
        pat->data[11][s].on = 1;
        pat->data[11][s].velocity = 42;
    }
    /* 第 15 步的底鼓滚奏：直接展示 ratchet 功能 */
    pat->data[0][15].on = 1;
    pat->data[0][15].velocity = 88;
    pat->data[0][15].ratchet = 3;
}

void xu_project_default(XuProject* p)
{
    int i, j;
    memset(p, 0, sizeof(*p));
    strcpy(p->name, "XuMou Xu808 Project");
    p->seed = 0x808808;
    p->current = 0;

    for (i = 0; i < XU_PADS; i++)
    {
        p->pads[i].tune = 0.50f;
        p->pads[i].decay = 0.50f;
        p->pads[i].tone = 0.50f;
        p->pads[i].snappy = 0.55f;
        p->pads[i].tail = 0.45f;
        p->pads[i].level = 0.75f;
        p->pads[i].pan = 0.50f;
        p->pads[i].mute = 0;
        p->pads[i].solo = 0;
    }
    p->pads[0].decay = 0.55f; /* 底鼓稍长 */
    p->pads[0].tone = 0.45f;
    p->pads[1].decay = 0.45f;

    for (i = 0; i < XU_PATTERNS; i++)
    {
        XuPattern* pat = &p->patterns[i];
        memset(pat, 0, sizeof(*pat));
        sprintf(pat->name, "PATTERN %d", i + 1);
        pat->steps = 16;
        for (j = 0; j < XU_PADS; j++)
        {
            pat->trackLen[j] = 16;
            /* 每格默认力度/概率/ratchet，保证 UI 显示正常 */
            for (int s = 0; s < XU_STEPS_MAX; s++)
            {
                pat->data[j][s].velocity = 100;
                pat->data[j][s].prob = 100;
                pat->data[j][s].ratchet = 1;
            }
        }
    }
    xu_project_demo(&p->patterns[0]);

    p->master.volume = 0.80f;
    p->master.drive = 1.35f;
    p->master.lpCutoff = 18000.0f;
    p->master.lpQ = 0.90f;
    p->master.hpCutoff = 28.0f;
    p->master.hpQ = 0.70f;
    p->master.lpOn = 1;
    p->master.hpOn = 1;
    p->master.bpm = 120.0f;
    p->master.swing = 0.0f;
    p->master.beatsPerBar = 4;
    p->master.beatUnit = 4;
    p->master.songMode = 0;

    p->songCount = 2;
    p->song[0].pattern = 0;
    p->song[0].repeats = 4;
    p->song[1].pattern = 0;
    p->song[1].repeats = 4;
}

/* ---------------- 初始化 ---------------- */
void xu_engine_init(XuEngine* e, XuProject* p)
{
    int i, v, k;
    memset(e, 0, sizeof(*e));
    e->proj = p;
    e->rng = p->seed ? p->seed : 0x808808;
    e->samplePos = 0;
    e->pendingPattern = -1;
    e->soloOverride = -1;
    e->voices[0][0].pad = 0; /* 防止未初始化警告；下面统一设置 */
    for (i = 0; i < XU_PADS; i++)
    {
        for (v = 0; v < XU_MAX_VOICES; v++)
        {
            XuVoice* vo = &e->voices[i][v];
            memset(vo, 0, sizeof(*vo));
            vo->pad = i;
            /* 用确定性伪随机填噪声池：同一台机器每次启动音色都一致 */
            unsigned int st = (unsigned int)(0x808 + i * 7919);
            for (k = 0; k < 8192; k++)
            {
                st = st * 1664525u + 1013904223u;
                vo->noise[k] = (float)((int)(st >> 8) / 8388608.0 - 1.0);
            }
        }
    }
}

/* ---------------- 传输 ---------------- */
void xu_engine_play(XuEngine* e)
{
    if (e->playing) return;
    e->playing = 1;
    e->stepIndex = 0;
    e->globalStep = 0;
    e->cycleCount = 0;
    e->fillNow = 0;
    e->songIndex = 0;
    e->songRepeat = 0;
    e->nextStepAbs = e->samplePos;
    e->pendingCount = 0;
}

void xu_engine_stop(XuEngine* e)
{
    e->playing = 0;
    for (int i = 0; i < XU_PADS; i++)
        for (int v = 0; v < XU_MAX_VOICES; v++)
            xu_voice_kill(&e->voices[i][v], e->samplePos, 480); /* 10ms 淡出 */
    e->pendingCount = 0;
}

/* ---------------- 时钟 ---------------- */
static int step_len(const XuEngine* e)
{
    float bpm = e->proj->master.bpm;
    if (bpm < 40.0f) bpm = 40.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    int n = (int)(XU_SR * 60.0 / bpm / 4.0 + 0.5);
    return n < 64 ? 64 : n;
}

/* xorshift32：确定性随机，用于概率触发；无分配、极快 */
static unsigned int next_rand(XuEngine* e)
{
    unsigned int x = e->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    e->rng = x;
    return x;
}

static int cond_ok(const XuEngine* e, int cond)
{
    switch (cond)
    {
    case XU_COND_ALWAYS: return 1;
    case XU_COND_FILL: return e->fillNow;
    case XU_COND_NOTFILL: return !e->fillNow;
    case XU_COND_EVERY2: return (e->cycleCount % 2) == 0;
    case XU_COND_EVERY4: return (e->cycleCount % 4) == 0;
    default: return 1;
    }
}

/* 压入一个事件；队列满时丢弃最旧的一条，绝不覆盖内存 */
static void push_event(XuEvent* arr, int* count, long long t, int pad, float vel, int accent)
{
    if (*count >= XU_MAX_EVENTS)
    {
        memmove(arr, arr + 1, sizeof(XuEvent) * (XU_MAX_EVENTS - 1));
        (*count)--;
    }
    arr[*count].t = t;
    arr[*count].pad = pad;
    arr[*count].vel = vel;
    arr[*count].accent = accent;
    (*count)++;
}

/* 展开一个 16 分步 */
static void emit_step(XuEngine* e, long long stepAbs)
{
    XuPattern* pat = &e->proj->patterns[e->proj->current];
    int sl = step_len(e);
    int swingOff = 0;
    if (e->stepIndex & 1)
        swingOff = (int)(sl * e->proj->master.swing * 0.5);
    long long base = stepAbs + swingOff;

    for (int pad = 0; pad < XU_PADS; pad++)
    {
        int tl = pat->trackLen[pad];
        if (tl < 1) tl = 1;
        if (tl > 64) tl = 64;
        int idx = (int)(e->globalStep % tl);
        XuStep* st = &pat->data[pad][idx];
        if (!st->on) continue;
        if (!cond_ok(e, st->cond)) continue;
        if (st->prob == 0) continue;
        if (st->prob < 100 && (int)(next_rand(e) % 100) >= st->prob) continue;

        float vel = st->velocity / 127.0f;
        if (vel < 0.02f) vel = 0.02f;
        if (vel > 1.0f) vel = 1.0f;
        if (st->accent) vel *= 1.30f;
        if (vel > 1.25f) vel = 1.25f;

        int r = st->ratchet;
        if (r < 1) r = 1;
        if (r > 8) r = 8;
        for (int i = 0; i < r; i++)
        {
            long long t = base + st->micro + (long long)(i * (double)sl / r);
            float vv = vel * (float)pow(0.92, i);
            push_event(e->pending, &e->pendingCount, t, pad, vv, st->accent);
        }
    }
}

/* 步进推进：切 pattern / 歌曲链只在循环边界发生 */
static void advance_step(XuEngine* e)
{
    XuPattern* pat = &e->proj->patterns[e->proj->current];
    e->stepIndex++;
    e->globalStep++;
    if (e->stepIndex < (pat->steps > 0 ? pat->steps : 1)) return;

    e->stepIndex = 0;
    e->cycleCount++;
    e->fillNow = (e->cycleCount % 4) == 3;

    if (e->pendingPattern >= 0)
    {
        int n = e->pendingPattern;
        if (n >= XU_PATTERNS) n = XU_PATTERNS - 1;
        e->proj->current = n;
        e->pendingPattern = -1;
        e->songRepeat = 0;
        return;
    }
    if (e->proj->master.songMode && e->proj->songCount > 0)
    {
        XuSongEntry* en = &e->proj->song[e->songIndex % e->proj->songCount];
        e->songRepeat++;
        if (e->songRepeat >= (en->repeats > 0 ? en->repeats : 1))
        {
            e->songRepeat = 0;
            e->songIndex = (e->songIndex + 1) % e->proj->songCount;
            int np = e->proj->song[e->songIndex].pattern;
            if (np < 0) np = 0;
            if (np >= XU_PATTERNS) np = XU_PATTERNS - 1;
            e->proj->current = np;
        }
    }
}

/* 判断声部是否可闻（mute/solo 在混音阶段处理） */
static int audible(const XuEngine* e, int pad)
{
    if (e->soloOverride >= 0) return pad == e->soloOverride;
    int anySolo = 0;
    for (int i = 0; i < XU_PADS; i++)
        if (e->proj->pads[i].solo)
        {
            anySolo = 1;
            break;
        }
    const XuPadParams* p = &e->proj->pads[pad];
    if (p->mute) return 0;
    if (anySolo && !p->solo) return 0;
    return 1;
}

/* 触发一个 voice，并处理 choke 与抢占 */
static void trigger(XuEngine* e, int pad, long long when, float vel, int accent)
{
    int i;
    if (pad == 15) /* 闭镲掐开镲 */
        for (i = 0; i < XU_MAX_VOICES; i++)
            xu_voice_kill(&e->voices[14][i], when, 16);
    if (pad == 14) /* 开镲重触发先掐旧的 */
        for (i = 0; i < XU_MAX_VOICES; i++)
            xu_voice_kill(&e->voices[14][i], when, 16);

    XuVoice* target = NULL;
    for (i = 0; i < XU_MAX_VOICES; i++)
    {
        if (!e->voices[pad][i].active)
        {
            target = &e->voices[pad][i];
            break;
        }
    }
    if (!target)
    {
        target = &e->voices[pad][0];
        for (i = 1; i < XU_MAX_VOICES; i++)
            if (e->voices[pad][i].start < target->start)
                target = &e->voices[pad][i];
        xu_voice_kill(target, when, 64);
    }
    xu_voice_trigger(target, when, vel, accent);
}

/* 收集本块事件并按时间排序（插入排序，事件数很小） */
static void collect_events(XuEngine* e, long long b0, long long b1, XuEvent* out, int* outCount)
{
    int i, keep;
    *outCount = 0;

    /* 手点事件：保留“未来”的，取走“本块”的 */
    keep = 0;
    for (i = 0; i < e->manualCount; i++)
    {
        XuEvent ev = e->manual[i];
        if (ev.t < b1)
        {
            if (ev.t < b0) ev.t = b0;
            out[(*outCount)++] = ev;
        }
        else
        {
            e->manual[keep++] = ev;
        }
    }
    e->manualCount = keep;

    if (e->playing && !e->clockExternal)
    {
        int guard = 0;
        while (e->nextStepAbs < b1 && guard++ < 64)
        {
            emit_step(e, e->nextStepAbs > b0 ? e->nextStepAbs : b0);
            e->nextStepAbs += step_len(e);
            advance_step(e);
        }
    }

    keep = 0;
    for (i = 0; i < e->pendingCount; i++)
    {
        XuEvent ev = e->pending[i];
        if (ev.t < b1)
        {
            if (ev.t < b0) ev.t = b0;
            out[(*outCount)++] = ev;
        }
        else
        {
            e->pending[keep++] = ev;
        }
    }
    e->pendingCount = keep;

    for (i = 1; i < *outCount; i++)
    {
        XuEvent key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].t > key.t)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
}

/* 渲染 [a,b) 区间内的所有 voice */
static void render_voices(XuEngine* e, float* L, float* R, int n,
                          long long b0, long long a, long long b)
{
    if (b <= a) return;
    int count = (int)(b - a);
    for (int pad = 0; pad < XU_PADS; pad++)
    {
        if (!audible(e, pad)) continue;
        const XuPadParams* p = &e->proj->pads[pad];
        for (int v = 0; v < XU_MAX_VOICES; v++)
            if (e->voices[pad][v].active)
                xu_voice_render(&e->voices[pad][v], L, R, n, b0, b, p);
    }
}

/* --------------------------------------------------------------------------
 *  主渲染：整个程序唯一的声音出口。
 *  L/R 长度必须 >= n；函数内部不清零调用者的缓冲区之外的任何内存。
 * -------------------------------------------------------------------------- */
void xu_engine_render(XuEngine* e, int n, float* L, float* R)
{
    long long b0 = e->samplePos;
    long long b1 = b0 + n;
    memset(L, 0, sizeof(float) * n);
    memset(R, 0, sizeof(float) * n);

    static XuEvent evbuf[XU_MAX_EVENTS]; /* 静态：回调里零分配 */
    int evCount = 0;
    collect_events(e, b0, b1, evbuf, &evCount);

    long long cur = b0;
    for (int i = 0; i < evCount; i++)
    {
        XuEvent ev = evbuf[i];
        if (ev.t > cur)
        {
            render_voices(e, L, R, n, b0, cur, ev.t);
            cur = ev.t;
        }
        if (ev.t < b1) trigger(e, ev.pad, ev.t, ev.vel, ev.accent);
    }
    if (cur < b1) render_voices(e, L, R, n, b0, cur, b1);

    /* ---- 主效果链 ---- */
    XuMaster* m = &e->proj->master;
    if (m->hpOn)
    {
        xu_biquad_hp(&e->hpL, m->hpCutoff, m->hpQ);
        xu_biquad_hp(&e->hpR, m->hpCutoff, m->hpQ);
        for (int i = 0; i < n; i++)
        {
            L[i] = (float)xu_biquad_run(&e->hpL, L[i]);
            R[i] = (float)xu_biquad_run(&e->hpR, R[i]);
        }
    }
    if (m->lpOn)
    {
        xu_biquad_lp(&e->lpL, m->lpCutoff, m->lpQ);
        xu_biquad_lp(&e->lpR, m->lpCutoff, m->lpQ);
        for (int i = 0; i < n; i++)
        {
            L[i] = (float)xu_biquad_run(&e->lpL, L[i]);
            R[i] = (float)xu_biquad_run(&e->lpR, R[i]);
        }
    }
    float drive = m->drive < 1.0f ? 1.0f : (m->drive > 8.0f ? 8.0f : m->drive);
    float norm = (float)tanh(drive);
    float vol = m->volume < 0.0f ? 0.0f : (m->volume > 1.0f ? 1.0f : m->volume);
    float pl = 0, pr = 0;
    for (int i = 0; i < n; i++)
    {
        float xl = L[i], xr = R[i];
        if (drive > 1.001f)
        {
            xl = (float)tanh(xl * drive) / norm;
            xr = (float)tanh(xr * drive) / norm;
        }
        xl = xl / (1.0f + fabsf(xl)); /* 软限幅，永不超过 ±1 */
        xr = xr / (1.0f + fabsf(xr));
        xl *= vol;
        xr *= vol;
        L[i] = xl;
        R[i] = xr;
        if (fabsf(xl) > pl) pl = fabsf(xl);
        if (fabsf(xr) > pr) pr = fabsf(xr);
    }
    e->meterL = pl > e->meterL * 0.75f ? pl : e->meterL * 0.75f;
    e->meterR = pr > e->meterR * 0.75f ? pr : e->meterR * 0.75f;

    e->samplePos = b1;
    for (int p = 0; p < XU_PADS; p++)
        for (int v = 0; v < XU_MAX_VOICES; v++)
            if (e->voices[p][v].active && b0 - e->voices[p][v].start > 4LL * XU_SR)
                e->voices[p][v].active = 0;
}

/* ---------------- UI 调用 ---------------- */
void xu_engine_trigger_pad(XuEngine* e, int pad, int vel, int accent)
{
    if (pad < 0 || pad >= XU_PADS) return;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;
    push_event(e->manual, &e->manualCount, e->samplePos + 32, pad, vel / 127.0f, accent);
}

void xu_engine_request_pattern(XuEngine* e, int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= XU_PATTERNS) idx = XU_PATTERNS - 1;
    if (e->playing) e->pendingPattern = idx;
    else e->proj->current = idx;
}

void xu_engine_external_tick(XuEngine* e)
{
    if (!e->playing) return;
    e->clockPulse++;
    if (e->clockPulse < 6) return;
    e->clockPulse = 0;
    emit_step(e, e->samplePos);
    advance_step(e);
    e->nextStepAbs = e->samplePos + step_len(e);
}

/* ==========================================================================
 *  waveOut 输出
 * ========================================================================== */
typedef struct
{
    HWAVEOUT h;
    WAVEFORMATEX fmt;
    WAVEHDR hdr[4];
    short data[4][XU_BLOCK * 2];
    int next;
    volatile int running;
    XuEngine* eng;
    LARGE_INTEGER freq, t0;
} XuAudio;

static XuAudio g_audio;
static float g_L[XU_BLOCK], g_R[XU_BLOCK];

static void CALLBACK wave_cb(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h;
    (void)inst;
    (void)p1;
    (void)p2;
    if (msg != WOM_DONE || !g_audio.running) return;

    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    /* 渲染一块 */
    xu_engine_render(g_audio.eng, XU_BLOCK, g_L, g_R);
    /* 浮点转 int16，小端序 */
    for (int i = 0; i < XU_BLOCK; i++)
    {
        float l = g_L[i], r = g_R[i];
        if (l < -1.0f) l = -1.0f;
        if (l > 1.0f) l = 1.0f;
        if (r < -1.0f) r = -1.0f;
        if (r > 1.0f) r = 1.0f;
        g_audio.data[g_audio.next][i * 2] = (short)(l * 32767.0f);
        g_audio.data[g_audio.next][i * 2 + 1] = (short)(r * 32767.0f);
    }
    WAVEHDR* hdr = &g_audio.hdr[g_audio.next];
    g_audio.next = (g_audio.next + 1) & 3;
    waveOutWrite(g_audio.h, hdr, sizeof(WAVEHDR));

    QueryPerformanceCounter(&b);
    /* CPU 占用 = 渲染耗时 / 本块应有时间，慢速平滑 */
    double dt = (double)(b.QuadPart - a.QuadPart) / (double)g_audio.freq.QuadPart;
    double ideal = (double)XU_BLOCK / (double)XU_SR;
    g_audio.eng->cpu = g_audio.eng->cpu * 0.95f + (float)(dt / ideal) * 0.05f;
}

int xu_audio_start(XuEngine* e)
{
    if (g_audio.running) return 1;
    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.eng = e;
    QueryPerformanceFrequency(&g_audio.freq);

    g_audio.fmt.wFormatTag = WAVE_FORMAT_PCM;
    g_audio.fmt.nChannels = 2;
    g_audio.fmt.nSamplesPerSec = XU_SR;
    g_audio.fmt.wBitsPerSample = 16;
    g_audio.fmt.nBlockAlign = 4;
    g_audio.fmt.nAvgBytesPerSec = XU_SR * 4;
    g_audio.fmt.cbSize = 0;

    MMRESULT rc = waveOutOpen(&g_audio.h, WAVE_MAPPER, &g_audio.fmt,
                              (DWORD_PTR)wave_cb, 0, CALLBACK_FUNCTION);
    if (rc != MMSYSERR_NOERROR) return 0;

    for (int i = 0; i < 4; i++)
    {
        memset(&g_audio.hdr[i], 0, sizeof(WAVEHDR));
        g_audio.hdr[i].lpData = (LPSTR)g_audio.data[i];
        g_audio.hdr[i].dwBufferLength = XU_BLOCK * 4;
        waveOutPrepareHeader(g_audio.h, &g_audio.hdr[i], sizeof(WAVEHDR));
        /* 先填一块再写，否则开头会有半块静音 */
        xu_engine_render(e, XU_BLOCK, g_L, g_R);
        for (int k = 0; k < XU_BLOCK; k++)
        {
            float l = g_L[k], r = g_R[k];
            if (l < -1.0f) l = -1.0f;
            if (l > 1.0f) l = 1.0f;
            if (r < -1.0f) r = -1.0f;
            if (r > 1.0f) r = 1.0f;
            g_audio.data[i][k * 2] = (short)(l * 32767.0f);
            g_audio.data[i][k * 2 + 1] = (short)(r * 32767.0f);
        }
        waveOutWrite(g_audio.h, &g_audio.hdr[i], sizeof(WAVEHDR));
    }
    g_audio.next = 0;
    g_audio.running = 1;
    return 1;
}

void xu_audio_stop(void)
{
    if (!g_audio.running) return;
    g_audio.running = 0;
    waveOutReset(g_audio.h);
    for (int i = 0; i < 4; i++)
        waveOutUnprepareHeader(g_audio.h, &g_audio.hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_audio.h);
    g_audio.h = NULL;
}

int xu_audio_running(void)
{
    return g_audio.running;
}

/* ==========================================================================
 *  WAV 导出（离线渲染，复用同一 DSP）
 * ========================================================================== */
static int write_all(HANDLE f, const void* buf, DWORD n)
{
    DWORD wrote = 0;
    return WriteFile(f, buf, n, &wrote, NULL) && wrote == n;
}

/* 计算一首歌/一个 pattern 的总步数 */
static long long total_steps(XuProject* p, int song, int repeats)
{
    long long n = 0;
    if (song && p->songCount > 0)
    {
        for (int i = 0; i < p->songCount; i++)
        {
            int idx = p->song[i].pattern;
            if (idx < 0) idx = 0;
            if (idx >= XU_PATTERNS) idx = XU_PATTERNS - 1;
            int r = p->song[i].repeats > 0 ? p->song[i].repeats : 1;
            n += (long long)p->patterns[idx].steps * r;
        }
    }
    else
    {
        n = (long long)p->patterns[p->current].steps * (repeats > 0 ? repeats : 1);
    }
    return n > 0 ? n : 16;
}

int xu_export_wav(XuProject* p, const wchar_t* path, int song, int soloPad)
{
    XuEngine* e = (XuEngine*)calloc(1, sizeof(XuEngine));
    if (!e) return 0;
    xu_engine_init(e, p);
    e->soloOverride = soloPad;
    xu_engine_play(e);

    long long steps = total_steps(p, song, 1);
    int sl = step_len(e);
    long long frames = steps * sl + 3LL * XU_SR; /* 末尾 3 秒尾音 */
    long long dataBytes = frames * 4;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        free(e);
        return 0;
    }

    /* RIFF 头：先写占位，最后 Seek 回来补真实大小 */
    DWORD written = 0;
    BYTE hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    *(DWORD*)(hdr + 4) = (DWORD)(36 + dataBytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    *(DWORD*)(hdr + 16) = 16;
    *(WORD*)(hdr + 20) = 1;
    *(WORD*)(hdr + 22) = 2;
    *(DWORD*)(hdr + 24) = XU_SR;
    *(DWORD*)(hdr + 28) = XU_SR * 4;
    *(WORD*)(hdr + 32) = 4;
    *(WORD*)(hdr + 34) = 16;
    memcpy(hdr + 36, "data", 4);
    *(DWORD*)(hdr + 40) = (DWORD)dataBytes;
    WriteFile(f, hdr, 44, &written, NULL);

    float L[XU_BLOCK], R[XU_BLOCK];
    short out[XU_BLOCK * 2];
    long long done = 0;
    while (done < frames)
    {
        int n = (frames - done) < XU_BLOCK ? (int)(frames - done) : XU_BLOCK;
        xu_engine_render(e, n, L, R);
        for (int i = 0; i < n; i++)
        {
            float l = L[i], r = R[i];
            if (l < -1.0f) l = -1.0f;
            if (l > 1.0f) l = 1.0f;
            if (r < -1.0f) r = -1.0f;
            if (r > 1.0f) r = 1.0f;
            out[i * 2] = (short)(l * 32767.0f);
            out[i * 2 + 1] = (short)(r * 32767.0f);
        }
        if (!write_all(f, out, (DWORD)(n * 4)))
        {
            CloseHandle(f);
            free(e);
            return 0;
        }
        done += n;
    }
    CloseHandle(f);
    free(e);
    return 1;
}

/* ==========================================================================
 *  MIDI 导出
 * ========================================================================== */
typedef struct
{
    int tick, note, vel;
} XuNote;

static int cmp_note(const void* a, const void* b)
{
    const XuNote *x = (const XuNote*)a, *y = (const XuNote*)b;
    if (x->tick != y->tick) return x->tick - y->tick;
    return x->note - y->note;
}

static int vlq(BYTE* out, int value)
{
    if (value < 0) value = 0;
    BYTE tmp[5];
    int n = 0, i;
    tmp[n++] = (BYTE)(value & 0x7F);
    value >>= 7;
    while (value > 0)
    {
        tmp[n++] = (BYTE)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

int xu_export_midi(XuProject* p, const wchar_t* path, int song)
{
    int ppq = 480;
    double bpm = p->master.bpm < 40 ? 40 : (p->master.bpm > 300 ? 300 : p->master.bpm);
    double stepSamples = XU_SR * 60.0 / bpm / 4.0;
    int maxNotes = 16 * 64 * 8 * 16;
    XuNote* notes = (XuNote*)malloc(sizeof(XuNote) * maxNotes);
    if (!notes) return 0;
    int count = 0;

    int seq[256], seqCount = 0;
    if (song && p->songCount > 0)
    {
        for (int i = 0; i < p->songCount; i++)
        {
            int idx = p->song[i].pattern;
            if (idx < 0) idx = 0;
            if (idx >= XU_PATTERNS) idx = XU_PATTERNS - 1;
            int r = p->song[i].repeats > 0 ? p->song[i].repeats : 1;
            for (int k = 0; k < r && seqCount < 256; k++) seq[seqCount++] = idx;
        }
    }
    else
    {
        seq[seqCount++] = p->current;
    }

    long long cursor = 0;
    for (int si = 0; si < seqCount; si++)
    {
        XuPattern* pat = &p->patterns[seq[si]];
        for (int s = 0; s < pat->steps; s++)
        {
            int swingTick = (s % 2) ? (int)(p->master.swing * 60.0) : 0;
            for (int pad = 0; pad < XU_PADS; pad++)
            {
                int tl = pat->trackLen[pad];
                if (tl < 1) tl = 1;
                if (tl > 64) tl = 64;
                int idx = (int)((cursor + s) % tl);
                XuStep* st = &pat->data[pad][idx];
                if (!st->on || st->prob == 0) continue;
                int vel = (int)(st->velocity * (st->accent ? 1.3f : 1.0f));
                if (vel < 1) vel = 1;
                if (vel > 127) vel = 127;
                int baseTick = (int)((cursor + s) * 120);
                int microTick = (int)(st->micro / stepSamples * 120.0);
                int r = st->ratchet < 1 ? 1 : (st->ratchet > 8 ? 8 : st->ratchet);
                for (int i = 0; i < r && count < maxNotes; i++)
                {
                    int tick = baseTick + swingTick + microTick + (int)(i * 120.0 / r);
                    if (tick < 0) tick = 0;
                    notes[count].tick = tick;
                    notes[count].note = XU_MIDI_NOTES[pad];
                    notes[count].vel = vel;
                    count++;
                }
            }
        }
        cursor += pat->steps;
    }
    qsort(notes, count, sizeof(XuNote), cmp_note);

    /* 构造轨道数据 */
    BYTE* track = (BYTE*)malloc(1024 * 1024);
    if (!track)
    {
        free(notes);
        return 0;
    }
    int tn = 0;
    const char* name = "XU-808 by XuMou";
    tn += vlq(track + tn, 0);
    track[tn++] = 0xFF;
    track[tn++] = 0x03;
    tn += vlq(track + tn, (int)strlen(name));
    memcpy(track + tn, name, strlen(name));
    tn += (int)strlen(name);

    int tempo = (int)(60000000.0 / bpm);
    tn += vlq(track + tn, 0);
    track[tn++] = 0xFF;
    track[tn++] = 0x51;
    track[tn++] = 0x03;
    track[tn++] = (BYTE)(tempo >> 16);
    track[tn++] = (BYTE)(tempo >> 8);
    track[tn++] = (BYTE)tempo;

    tn += vlq(track + tn, 0);
    track[tn++] = 0xFF;
    track[tn++] = 0x58;
    track[tn++] = 0x04;
    track[tn++] = (BYTE)p->master.beatsPerBar;
    int du = 0, bu = p->master.beatUnit;
    while (bu > 1)
    {
        bu >>= 1;
        du++;
    }
    track[tn++] = (BYTE)du;
    track[tn++] = 24;
    track[tn++] = 8;

    int last = 0;
    for (int i = 0; i < count; i++)
    {
        tn += vlq(track + tn, notes[i].tick - last);
        track[tn++] = 0x99;
        track[tn++] = (BYTE)notes[i].note;
        track[tn++] = (BYTE)notes[i].vel;
        tn += vlq(track + tn, 10);
        track[tn++] = 0x89;
        track[tn++] = (BYTE)notes[i].note;
        track[tn++] = 0x40;
        last = notes[i].tick + 10;
    }
    tn += vlq(track + tn, 0);
    track[tn++] = 0xFF;
    track[tn++] = 0x2F;
    track[tn++] = 0x00;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        free(track);
        free(notes);
        return 0;
    }
    DWORD w;
    WriteFile(f, "MThd", 4, &w, NULL);
    DWORD hlen = 6;
    WriteFile(f, &hlen, 4, &w, NULL);
    WORD one = 1, trackCount = 1, ppqW = (WORD)ppq;
    WriteFile(f, &one, 2, &w, NULL);
    WriteFile(f, &trackCount, 2, &w, NULL);
    WriteFile(f, &ppqW, 2, &w, NULL);
    WriteFile(f, "MTrk", 4, &w, NULL);
    DWORD tlen = (DWORD)tn;
    WriteFile(f, &tlen, 4, &w, NULL);
    WriteFile(f, track, (DWORD)tn, &w, NULL);
    CloseHandle(f);
    free(track);
    free(notes);
    return 1;
}

/* ==========================================================================
 *  工程保存/加载：Windows INI 文本格式（带 version 键，向后兼容）
 *
 *  这里没有用 JSON：纯 C 手写 JSON 解析器容易出安全漏洞，
 *  而 INI 有系统 API，稳定且可读。字段名与 JSON 版本完全一致，
 *  需要互通时可以用脚本转换。
 * ========================================================================== */
int xu_project_save(XuProject* p, const wchar_t* path)
{
    wchar_t buf[256];
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(f); /* 先建空文件，WritePrivateProfileString 才能写 */

    WritePrivateProfileStringW(L"meta", L"format", L"XU808", path);
    WritePrivateProfileStringW(L"meta", L"version", L"1", path);
    wsprintfW(buf, L"%hs", p->name);
    WritePrivateProfileStringW(L"meta", L"name", buf, path);
    wsprintfW(buf, L"%d", p->current);
    WritePrivateProfileStringW(L"meta", L"current", buf, path);
    wsprintfW(buf, L"%u", p->seed);
    WritePrivateProfileStringW(L"meta", L"seed", buf, path);

    XuMaster* m = &p->master;
    wsprintfW(buf, L"%.6f", m->volume);
    WritePrivateProfileStringW(L"master", L"volume", buf, path);
    wsprintfW(buf, L"%.6f", m->drive);
    WritePrivateProfileStringW(L"master", L"drive", buf, path);
    wsprintfW(buf, L"%.2f", m->lpCutoff);
    WritePrivateProfileStringW(L"master", L"lpCutoff", buf, path);
    wsprintfW(buf, L"%.4f", m->lpQ);
    WritePrivateProfileStringW(L"master", L"lpQ", buf, path);
    wsprintfW(buf, L"%.2f", m->hpCutoff);
    WritePrivateProfileStringW(L"master", L"hpCutoff", buf, path);
    wsprintfW(buf, L"%.4f", m->hpQ);
    WritePrivateProfileStringW(L"master", L"hpQ", buf, path);
    wsprintfW(buf, L"%d", m->lpOn);
    WritePrivateProfileStringW(L"master", L"lpOn", buf, path);
    wsprintfW(buf, L"%d", m->hpOn);
    WritePrivateProfileStringW(L"master", L"hpOn", buf, path);
    wsprintfW(buf, L"%.3f", m->bpm);
    WritePrivateProfileStringW(L"master", L"bpm", buf, path);
    wsprintfW(buf, L"%.4f", m->swing);
    WritePrivateProfileStringW(L"master", L"swing", buf, path);
    wsprintfW(buf, L"%d", m->beatsPerBar);
    WritePrivateProfileStringW(L"master", L"beatsPerBar", buf, path);
    wsprintfW(buf, L"%d", m->beatUnit);
    WritePrivateProfileStringW(L"master", L"beatUnit", buf, path);
    wsprintfW(buf, L"%d", m->songMode);
    WritePrivateProfileStringW(L"master", L"songMode", buf, path);

    for (int i = 0; i < XU_PADS; i++)
    {
        wchar_t sec[32];
        XuPadParams* pd = &p->pads[i];
        wsprintfW(sec, L"pad%d", i);
        wsprintfW(buf, L"%.4f", pd->tune);
        WritePrivateProfileStringW(sec, L"tune", buf, path);
        wsprintfW(buf, L"%.4f", pd->decay);
        WritePrivateProfileStringW(sec, L"decay", buf, path);
        wsprintfW(buf, L"%.4f", pd->tone);
        WritePrivateProfileStringW(sec, L"tone", buf, path);
        wsprintfW(buf, L"%.4f", pd->snappy);
        WritePrivateProfileStringW(sec, L"snappy", buf, path);
        wsprintfW(buf, L"%.4f", pd->tail);
        WritePrivateProfileStringW(sec, L"tail", buf, path);
        wsprintfW(buf, L"%.4f", pd->level);
        WritePrivateProfileStringW(sec, L"level", buf, path);
        wsprintfW(buf, L"%.4f", pd->pan);
        WritePrivateProfileStringW(sec, L"pan", buf, path);
        wsprintfW(buf, L"%d", pd->mute);
        WritePrivateProfileStringW(sec, L"mute", buf, path);
        wsprintfW(buf, L"%d", pd->solo);
        WritePrivateProfileStringW(sec, L"solo", buf, path);
    }

    for (int i = 0; i < XU_PATTERNS; i++)
    {
        XuPattern* pat = &p->patterns[i];
        wchar_t sec[32];
        wsprintfW(sec, L"pat%d", i);
        wsprintfW(buf, L"%hs", pat->name);
        WritePrivateProfileStringW(sec, L"name", buf, path);
        wsprintfW(buf, L"%d", pat->steps);
        WritePrivateProfileStringW(sec, L"steps", buf, path);
        wsprintfW(buf, L"%d", pat->fill);
        WritePrivateProfileStringW(sec, L"fill", buf, path);

        /* trackLen：16 个数字用逗号连接 */
        buf[0] = 0;
        for (int t = 0; t < XU_PADS; t++)
        {
            wchar_t tmp[16];
            wsprintfW(tmp, L"%d,", pat->trackLen[t]);
            wcscat(buf, tmp);
        }
        WritePrivateProfileStringW(sec, L"trackLen", buf, path);

        /* 每轨一行：64 组 "on,vel,acc,prob,micro,rat,cond;" */
        for (int pad = 0; pad < XU_PADS; pad++)
        {
            wchar_t key[16];
            wsprintfW(key, L"t%d", pad);
            buf[0] = 0;
            for (int s = 0; s < XU_STEPS_MAX; s++)
            {
                XuStep* st = &pat->data[pad][s];
                wchar_t tmp[64];
                wsprintfW(tmp, L"%d,%d,%d,%d,%d,%d,%d;",
                          st->on, st->velocity, st->accent, st->prob,
                          st->micro, st->ratchet, st->cond);
                wcscat(buf, tmp);
            }
            WritePrivateProfileStringW(sec, key, buf, path);
        }
    }

    wsprintfW(buf, L"%d", p->songCount);
    WritePrivateProfileStringW(L"song", L"count", buf, path);
    for (int i = 0; i < p->songCount && i < 64; i++)
    {
        wchar_t key[16];
        wsprintfW(key, L"e%d", i);
        wsprintfW(buf, L"%d,%d", p->song[i].pattern, p->song[i].repeats);
        WritePrivateProfileStringW(L"song", key, buf, path);
    }
    return 1;
}

/* 从 INI 读一个 float，带默认值 */
static float get_float(const wchar_t* sec, const wchar_t* key, const wchar_t* path, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, path);
    if (!buf[0]) return def;
    return (float)_wtof(buf);
}

static int get_int(const wchar_t* sec, const wchar_t* key, const wchar_t* path, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, path);
}

/*
 * 自己写一个宽字符分词器。
 * 为什么不用 wcstok：MinGW 默认跟 MSVCRT 的 2 参数版本走，
 * 而 C99 是 3 参数（带上下文指针），两者签名冲突会直接编译失败。
 * 这个版本只依赖 wcsspn/wcscspn，跨编译器稳定。
 */
static wchar_t* next_token(wchar_t** state, const wchar_t* delim)
{
    wchar_t* s = *state;
    if (!s) return NULL;
    s += wcsspn(s, delim); /* 跳过前导分隔符 */
    if (!*s)
    {
        *state = NULL;
        return NULL;
    }
    wchar_t* e = s + wcscspn(s, delim); /* 找到下一个分隔符 */
    if (*e)
    {
        *e = 0;
        *state = e + 1;
    }
    else *state = NULL;
    return s;
}

int xu_project_load(XuProject* p, const wchar_t* path)
{
    wchar_t buf[8192];
    /* 先恢复默认值，再用文件内容覆盖，保证缺字段时不会读越界 */
    xu_project_default(p);

    wsprintfW(buf, L"%hs", p->name);
    GetPrivateProfileStringW(L"meta", L"name", buf, buf, 64, path);
    p->name[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, p->name, 64, NULL, NULL);
    p->current = get_int(L"meta", L"current", path, 0);
    p->seed = (unsigned int)GetPrivateProfileIntW(L"meta", L"seed", 0x808808, path);
    if (p->current < 0) p->current = 0;
    if (p->current >= XU_PATTERNS) p->current = XU_PATTERNS - 1;

    XuMaster* m = &p->master;
    m->volume = get_float(L"master", L"volume", path, m->volume);
    m->drive = get_float(L"master", L"drive", path, m->drive);
    m->lpCutoff = get_float(L"master", L"lpCutoff", path, m->lpCutoff);
    m->lpQ = get_float(L"master", L"lpQ", path, m->lpQ);
    m->hpCutoff = get_float(L"master", L"hpCutoff", path, m->hpCutoff);
    m->hpQ = get_float(L"master", L"hpQ", path, m->hpQ);
    m->lpOn = get_int(L"master", L"lpOn", path, m->lpOn);
    m->hpOn = get_int(L"master", L"hpOn", path, m->hpOn);
    m->bpm = get_float(L"master", L"bpm", path, m->bpm);
    m->swing = get_float(L"master", L"swing", path, m->swing);
    m->beatsPerBar = get_int(L"master", L"beatsPerBar", path, m->beatsPerBar);
    m->beatUnit = get_int(L"master", L"beatUnit", path, m->beatUnit);
    m->songMode = get_int(L"master", L"songMode", path, m->songMode);
    if (m->bpm < 40) m->bpm = 40;
    if (m->bpm > 300) m->bpm = 300;

    for (int i = 0; i < XU_PADS; i++)
    {
        wchar_t sec[32];
        XuPadParams* pd = &p->pads[i];
        wsprintfW(sec, L"pad%d", i);
        pd->tune = get_float(sec, L"tune", path, pd->tune);
        pd->decay = get_float(sec, L"decay", path, pd->decay);
        pd->tone = get_float(sec, L"tone", path, pd->tone);
        pd->snappy = get_float(sec, L"snappy", path, pd->snappy);
        pd->tail = get_float(sec, L"tail", path, pd->tail);
        pd->level = get_float(sec, L"level", path, pd->level);
        pd->pan = get_float(sec, L"pan", path, pd->pan);
        pd->mute = get_int(sec, L"mute", path, pd->mute);
        pd->solo = get_int(sec, L"solo", path, pd->solo);
    }

    for (int i = 0; i < XU_PATTERNS; i++)
    {
        XuPattern* pat = &p->patterns[i];
        wchar_t sec[32];
        wsprintfW(sec, L"pat%d", i);
        GetPrivateProfileStringW(sec, L"name", L"", buf, 32, path);
        if (buf[0])
        {
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, pat->name, 32, NULL, NULL);
        }
        pat->steps = get_int(sec, L"steps", path, pat->steps);
        if (pat->steps < 1) pat->steps = 1;
        if (pat->steps > 64) pat->steps = 64;
        pat->fill = get_int(sec, L"fill", path, pat->fill);

        GetPrivateProfileStringW(sec, L"trackLen", L"", buf, 8192, path);
        if (buf[0])
        {
            wchar_t* ctx = buf;
            wchar_t* tok = next_token(&ctx, L",");
            for (int t = 0; t < XU_PADS && tok; t++)
            {
                int v = _wtoi(tok);
                if (v < 1) v = 1;
                if (v > 64) v = 64;
                pat->trackLen[t] = (unsigned char)v;
                tok = next_token(&ctx, L",");
            }
        }
        for (int pad = 0; pad < XU_PADS; pad++)
        {
            wchar_t key[16];
            wsprintfW(key, L"t%d", pad);
            GetPrivateProfileStringW(sec, key, L"", buf, 8192, path);
            if (!buf[0]) continue;
            wchar_t* ctx = buf;
            wchar_t* tok = next_token(&ctx, L";");
            for (int s = 0; s < XU_STEPS_MAX && tok; s++)
            {
                int vals[7] = {0, 100, 0, 100, 0, 1, 0};
                wchar_t* c2 = tok;
                wchar_t* f = next_token(&c2, L",");
                for (int k = 0; k < 7 && f; k++)
                {
                    vals[k] = _wtoi(f);
                    f = next_token(&c2, L",");
                }
                XuStep* st = &pat->data[pad][s];
                st->on = (unsigned char)(vals[0] ? 1 : 0);
                st->velocity = (unsigned char)(vals[1] < 1 ? 1 : (vals[1] > 127 ? 127 : vals[1]));
                st->accent = (unsigned char)(vals[2] ? 1 : 0);
                st->prob = (unsigned char)(vals[3] < 0 ? 0 : (vals[3] > 100 ? 100 : vals[3]));
                st->micro = (short)(vals[4] < -2400 ? -2400 : (vals[4] > 2400 ? 2400 : vals[4]));
                st->ratchet = (unsigned char)(vals[5] < 1 ? 1 : (vals[5] > 8 ? 8 : vals[5]));
                st->cond = (unsigned char)(vals[6] < 0 ? 0 : (vals[6] > 4 ? 4 : vals[6]));
                tok = next_token(&ctx, L";");
            }
        }
    }

    p->songCount = get_int(L"song", L"count", path, p->songCount);
    if (p->songCount < 0) p->songCount = 0;
    if (p->songCount > 64) p->songCount = 64;
    for (int i = 0; i < p->songCount; i++)
    {
        wchar_t key[16];
        wsprintfW(key, L"e%d", i);
        GetPrivateProfileStringW(L"song", key, L"0,1", buf, 64, path);
        int a = 0, b = 1;
        swscanf(buf, L"%d,%d", &a, &b);
        p->song[i].pattern = a < 0 ? 0 : (a >= XU_PATTERNS ? XU_PATTERNS - 1 : a);
        p->song[i].repeats = b < 1 ? 1 : b;
    }
    return 1;
}