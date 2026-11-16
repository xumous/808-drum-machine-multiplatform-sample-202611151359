/* ==========================================================================
 *  dsp.c —— 十六声部模拟鼓合成（纯 C 版）
 *
 *  徐某的原则：音频线程里绝不做 malloc/free，绝不调用系统 API，
 *  绝不让异常（C 里就是 longjmp/崩溃）发生。因此：
 *    · 所有 voice、噪声池、延迟线都在 engine_init 时一次性准备好；
 *    · 每个样本只做 sin/exp/tanh 和几条乘加；
 *    · 包络是“年龄 t”的解析函数，不需要逐样本状态机。
 *
 *  最后一个好处非常关键：声音到时间自然归零，不会像循环采样那样
 *  “上一个音还没关，下一个又叠上来”，永远不会越积越多。
 * ========================================================================== */
#include "xu808.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

const char* XU_CODES[XU_PADS] = {
    "BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"
};
const wchar_t* XU_NAMES_CN[XU_PADS] = {
    L"底鼓", L"军鼓", L"低嗵", L"中嗵", L"高嗵", L"低康加", L"中康加", L"高康加",
    L"边击", L"击木", L"拍手", L"沙锤", L"牛铃", L"吊镲", L"开镲", L"闭镲"
};
const int XU_MIDI_NOTES[XU_PADS] = {
    36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42
};

/* 小工具：把 x 限制在 [lo,hi]，避免参数异常导致 tan() 爆掉 */
static double clampd(double x, double lo, double hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* ---------------- RBJ 低通 ---------------- */
void xu_biquad_lp(XuBiquad* f, double fc, double q)
{
    fc = clampd(fc, 20.0, XU_SR * 0.49);
    q = clampd(q, 0.1, 20.0);
    /* 参数几乎没变就不重算：三角函数在实时线程里能省则省 */
    if (f->kind == 1 && fabs(fc - f->fc) < 0.5 && fabs(q - f->q) < 1e-3) return;
    double w0 = 2.0 * 3.14159265358979323846 * fc / XU_SR;
    double cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = (1.0 - cw) * 0.5 / a0;
    f->b1 = (1.0 - cw) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cw) / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->kind = 1;
    f->fc = fc;
    f->q = q;
}

/* ---------------- RBJ 高通 ---------------- */
void xu_biquad_hp(XuBiquad* f, double fc, double q)
{
    fc = clampd(fc, 10.0, XU_SR * 0.49);
    q = clampd(q, 0.1, 20.0);
    if (f->kind == 2 && fabs(fc - f->fc) < 0.5 && fabs(q - f->q) < 1e-3) return;
    double w0 = 2.0 * 3.14159265358979323846 * fc / XU_SR;
    double cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = (1.0 + cw) * 0.5 / a0;
    f->b1 = -(1.0 + cw) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cw) / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->kind = 2;
    f->fc = fc;
    f->q = q;
}

/* ---------------- RBJ 带通（0dB 峰值） ---------------- */
void xu_biquad_bp(XuBiquad* f, double fc, double q)
{
    fc = clampd(fc, 20.0, XU_SR * 0.49);
    q = clampd(q, 0.1, 30.0);
    if (f->kind == 3 && fabs(fc - f->fc) < 0.5 && fabs(q - f->q) < 1e-3) return;
    double w0 = 2.0 * 3.14159265358979323846 * fc / XU_SR;
    double cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0;
    f->b2 = -alpha / a0;
    f->a1 = (-2.0 * cw) / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->kind = 3;
    f->fc = fc;
    f->q = q;
}

/* ---------------- 执行一次差分方程（DF2T） ---------------- */
double xu_biquad_run(XuBiquad* f, double x)
{
    double y = f->b0 * x + f->z1; /* 当前输出 */
    f->z1 = f->b1 * x - f->a1 * y + f->z2; /* 更新状态 1 */
    f->z2 = f->b2 * x - f->a2 * y; /* 更新状态 2 */
    return y;
}

/* ---------------- 触发一个 voice ---------------- */
void xu_voice_trigger(XuVoice* v, long long when, float gain, int accent)
{
    v->active = 1;
    v->start = when;
    v->stop = 0x7FFFFFFFFFFFFFFFLL; /* 默认永不强停 */
    v->gain = gain;
    v->accent = accent;
    v->fadeLen = 64;
    v->npos = 0;
    /* 清空滤波器记忆，避免上一槌的残余造成“噗” */
    v->bp.z1 = v->bp.z2 = 0.0;
    v->hp.z1 = v->hp.z2 = 0.0;
    v->bp2.z1 = v->bp2.z2 = 0.0;
}

/* ---------------- 掐断（choke / voice stealing） ---------------- */
void xu_voice_kill(XuVoice* v, long long when, int fade)
{
    if (!v->active) return;
    if (fade < 1) fade = 1;
    /* stop 必须在 when 之后留出 fade 长度，淡出斜坡才有地方发生；
       否则 stop=when 会让未来样本全部越界，等于硬切。 */
    if (when + fade < v->stop) v->stop = when + fade;
    v->fadeLen = fade;
}

/* 从循环噪声池取一个样本（零分配） */
static double take_noise(XuVoice* v)
{
    if (v->npos >= 8192) v->npos = 0;
    return (double)v->noise[v->npos++];
}

/* 方波：sin>=0 取 +1，否则 -1。比 signbit 更直观 */
static double square(double phase)
{
    return sin(phase) >= 0.0 ? 1.0 : -1.0;
}

/* --------------------------------------------------------------------------
 *  核心合成函数：给定“年龄 t”，返回该声部此刻的样本值。
 *  pad 分派与所有公式必须与 Python/C# 版逐项对应。
 * -------------------------------------------------------------------------- */
static double synth_sample(XuVoice* v, double t, const XuPadParams* p)
{
    int pad = v->pad;

    /* ================= 0 底鼓 ================= */
    if (pad == 0)
    {
        double fEnd = 40.0 + p->tune * 40.0;
        double fStart = fEnd * (1.6 + p->tone * 1.9);
        double tau = 0.018 + (1.0 - p->decay) * 0.012;
        double df = fStart - fEnd;
        /* 频率积分 → 相位；这是“鼓皮张力快速释放”的数学表达 */
        double phase = 2.0 * 3.14159265358979323846 *
            (fEnd * t + df * tau * (1.0 - exp(-t / tau)));
        double dec = (0.12 + p->decay * 1.48) * (v->accent ? 1.18 : 1.0);
        double amp = exp(-t / dec);
        double n = take_noise(v);
        xu_biquad_hp(&v->hp, 1800.0 + p->tone * 3000.0, 0.8);
        double click = xu_biquad_run(&v->hp, n) * exp(-t / 0.0035) *
            (0.06 + p->tone * 0.32);
        double body = tanh(sin(phase) * (1.15 + p->decay * 1.2)) * 0.92;
        return body * amp + click;
    }

    /* ================= 1 军鼓 ================= */
    if (pad == 1)
    {
        double f1 = 150.0 + p->tune * 110.0;
        double f2 = f1 * (1.78 + p->tone * 0.22);
        double decO = 0.055 + p->decay * 0.14;
        double osc = 0.55 * sin(2.0 * 3.14159265358979323846 * f1 * t) * exp(-t / decO)
            + 0.34 * sin(2.0 * 3.14159265358979323846 * f2 * t) * exp(-t / (decO * 0.72));
        double n = take_noise(v);
        xu_biquad_bp(&v->bp, 1200.0 + p->snappy * 3800.0, 0.9 + p->tone * 0.8);
        double nz = xu_biquad_run(&v->bp, n);
        double decN = (0.075 + p->decay * 0.16) * (v->accent ? 0.85 : 1.0);
        nz *= exp(-t / decN) * (0.25 + p->snappy * 0.7);
        return osc * 0.7 + nz;
    }

    /* ================= 2..7 嗵鼓 / 康加 ================= */
    if (pad >= 2 && pad <= 7)
    {
        double baseF, decayS, clickAmt, sweep;
        if (pad <= 4)
        {
            baseF = (pad == 2) ? 80.0 : (pad == 3) ? 120.0 : 170.0;
            decayS = 0.15 + p->decay * 0.75;
            clickAmt = 0.04 + p->tone * 0.22;
            sweep = 1.9;
        }
        else
        {
            baseF = (pad == 5) ? 220.0 : (pad == 6) ? 300.0 : 400.0;
            decayS = 0.10 + p->decay * 0.55;
            clickAmt = 0.02 + p->tone * 0.12;
            sweep = 1.35;
        }
        double f0 = baseF * (0.6 + p->tune);
        double tau = (pad <= 4) ? 0.035 : 0.020;
        double phase = 2.0 * 3.14159265358979323846 *
            (f0 * t + f0 * (sweep - 1.0) * tau * (1.0 - exp(-t / tau)));
        double amp = exp(-t / decayS);
        double n = take_noise(v);
        xu_biquad_hp(&v->hp, 2500.0, 0.7);
        double click = xu_biquad_run(&v->hp, n) * exp(-t / 0.004) * clickAmt;
        return sin(phase) * amp * 0.9 + click;
    }

    /* ================= 8 边击 ================= */
    if (pad == 8)
    {
        double f = 1200.0 + p->tune * 1400.0;
        double dec = 0.012 + p->decay * 0.05;
        double sq = square(2.0 * 3.14159265358979323846 * f * t);
        xu_biquad_bp(&v->bp, f, 2.2);
        double body = xu_biquad_run(&v->bp, sq) * exp(-t / dec);
        double n = take_noise(v);
        xu_biquad_hp(&v->hp, 3000.0, 0.8);
        double tick = xu_biquad_run(&v->hp, n) * exp(-t / 0.004) * 0.35;
        return body * 0.8 + tick;
    }

    /* ================= 9 击木 ================= */
    if (pad == 9)
    {
        double f = 2000.0 + p->tune * 1000.0;
        double dec = 0.018 + p->decay * 0.035;
        return sin(2.0 * 3.14159265358979323846 * f * t) * exp(-t / dec) * 0.9;
    }

    /* ================= 10 拍手 ================= */
    if (pad == 10)
    {
        double n = take_noise(v);
        xu_biquad_bp(&v->bp, 800.0 + p->tone * 1700.0, 1.1);
        double nz = xu_biquad_run(&v->bp, n);
        double env = 0.0;
        int k;
        for (k = 0; k < 4; k++)
        {
            double tk = k * 0.009;
            if (t >= tk) env += exp(-(t - tk) / 0.011) * 0.55;
        }
        double tail = exp(-t / (0.05 + p->decay * 0.35)) * (0.15 + p->tail * 0.55);
        return nz * (env + tail);
    }

    /* ================= 11 沙锤 ================= */
    if (pad == 11)
    {
        double n = take_noise(v);
        xu_biquad_hp(&v->hp, 4000.0 + p->tone * 5000.0, 0.9);
        double nz = xu_biquad_run(&v->hp, n);
        double env = (1.0 - exp(-t / 0.0015)) * exp(-t / (0.018 + p->decay * 0.12));
        return nz * env * 0.85;
    }

    /* ================= 12 牛铃 ================= */
    if (pad == 12)
    {
        double f = 400.0 + p->tune * 300.0;
        double sq1 = square(2.0 * 3.14159265358979323846 * f * t);
        double sq2 = square(2.0 * 3.14159265358979323846 * f * 1.48 * t);
        xu_biquad_bp(&v->bp, 2400.0 + p->tone * 1200.0, 1.4);
        double body = xu_biquad_run(&v->bp, sq1 * 0.5 + sq2 * 0.5);
        double dec = 0.05 + p->decay * 0.35;
        return body * exp(-t / dec) * 0.85;
    }

    /* ================= 13..15 镲族 ================= */
    {
        /* 6 个非谐和方波叠加，是 808 金属声的核心技巧 */
        static const double ratios[6] = {1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523};
        double root = 320.0 * (0.75 + p->tune * 0.5);
        double metal = 0.0;
        int i;
        for (i = 0; i < 6; i++)
            metal += square(2.0 * 3.14159265358979323846 * root * ratios[i] * t);
        metal /= 6.0;
        xu_biquad_hp(&v->hp, 5200.0 + p->tone * 2600.0, 0.8);
        metal = xu_biquad_run(&v->hp, metal);
        xu_biquad_bp(&v->bp, 7600.0 + p->tone * 3600.0, 0.9);
        metal = xu_biquad_run(&v->bp, metal);
        double dec = (pad == 15)
                         ? 0.028 + p->decay * 0.20
                         : (pad == 14)
                         ? 0.12 + p->decay * 0.65
                         : 0.25 + p->decay * 1.75;
        double env = exp(-t / dec);
        double out = metal * env;
        if (pad == 13)
        {
            double n = take_noise(v);
            xu_biquad_bp(&v->bp2, 10000.0, 0.7);
            out += xu_biquad_run(&v->bp2, n) * env * 0.12;
        }
        return out * 0.75;
    }
}

/* --------------------------------------------------------------------------
 *  把一个 voice 在 [b0,b1) 内渲染进左右声道。
 *  注意：这里不 new、不 malloc、不调用任何可能阻塞的函数。
 * -------------------------------------------------------------------------- */
void xu_voice_render(XuVoice* v, float* L, float* R, int n,
                     long long b0, long long b1, const XuPadParams* p)
{
    if (!v->active) return;
    long long a = (b0 > v->start) ? b0 : v->start;
    long long b = (b1 < v->stop) ? b1 : v->stop;
    if (b <= a)
    {
        if (b1 > v->stop) v->active = 0;
        return;
    }
    int m = (int)(b - a);
    int off = (int)(a - b0);
    if (m <= 0 || off >= n) return;

    /* 等功率声像：pan=0.5 时左右各 0.707，总功率恒定 */
    double pan = clampd(p->pan, 0.0, 1.0);
    double lg = cos(pan * 3.14159265358979323846 * 0.5);
    double rg = sin(pan * 3.14159265358979323846 * 0.5);
    double g = (double)v->gain * (p->level > 0 ? p->level : 0) * 0.42;

    int i;
    for (i = 0; i < m; i++)
    {
        long long abs = a + i;
        double t = (double)(abs - v->start) / (double)XU_SR;
        double s = synth_sample(v, t, p);
        /* 掐断淡出：距离 stop 越近，增益越小，避免硬切爆音 */
        if (v->stop < 0x7FFFFFFFFFFFFFFFLL)
        {
            long long dist = v->stop - abs;
            if (dist < v->fadeLen)
                s *= (dist <= 0) ? 0.0 : (double)dist / (double)v->fadeLen;
        }
        L[off + i] += (float)(s * g * lg);
        R[off + i] += (float)(s * g * rg);
    }
    /* 最多活 4 秒，到点无条件回收，保证不会无限堆叠 */
    if (b1 - v->start > 4LL * XU_SR) v->active = 0;
}