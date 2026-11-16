// ============================================================================
//  Xu808 Dsp.cs —— 十六声部模拟鼓合成器（C# 版）
//
//  徐某把这部分写成“纯函数 + 轻状态”：
//    · 包络全部是时间的解析函数（Exp/Sin），所以不需要逐样本维护 ADSR 状态；
//    · 唯一需要跨块保留的是滤波器 zi 和噪声读取位置；
//    · 每次敲击只重置 start/stop/gain，不重新分配任何数组。
//
//  这样做的直接好处：上一槌声音到时间自然结束，绝不可能“越叠越多”；
//  同时 CPU 占用可预测，16 声部 × 4 复音也不会突然爆表。
// ============================================================================

using System;
using System.Runtime.CompilerServices;

namespace Xu808
{
    /// <summary>全局音频常量。所有语言版本必须一致。</summary>
    public static class XuConst
    {
        public const int SampleRate = 48000;   // 采样率
        public const int PadCount = 16;        // 声部数
        public const int MaxVoices = 4;        // 每个声部的复音数

        // 声部代号与中文名（下标即编号，任何移植都不得改动顺序）
        public static readonly string[] Codes =
        { "BD","SD","LT","MT","HT","LC","MC","HC","RS","CL","CP","MA","CB","CY","OH","CH" };
        public static readonly string[] NamesCn =
        { "底鼓","军鼓","低嗵","中嗵","高嗵","低康加","中康加","高康加",
          "边击","击木","拍手","沙锤","牛铃","吊镲","开镲","闭镲" };
        public static readonly int[] MidiNotes =
        { 36,38,45,47,50,64,62,60,37,75,39,70,56,49,46,42 };
    }

    /// <summary>
    /// RBJ 双二阶滤波器（Direct Form II Transposed）。
    /// 知识点：音频里所有“低通/高通/带通”都可以化成同一条差分方程，
    /// 只是系数不同；lfilter 的递归状态就是“滤波器记忆”。
    /// </summary>
    public sealed class XuBiquad
    {
        private double b0 = 1, b1, b2, a1, a2;  // 系数（已归一化，a0 恒为 1）
        private double z1, z2;                   // 两条延迟线的状态
        private string kind = "bypass";
        private double lastFc, lastQ;

        /// <summary>低通：保留低频。fc 为截止频率，q 为品质因数。</summary>
        public void SetLowPass(double fc, double q)
        {
            fc = Math.Clamp(fc, 20.0, XuConst.SampleRate * 0.49);
            q = Math.Clamp(q, 0.1, 20.0);
            if (kind == "lp" && Math.Abs(fc - lastFc) < 0.5 && Math.Abs(q - lastQ) < 1e-3) return;
            double w0 = 2 * Math.PI * fc / XuConst.SampleRate;
            double cw = Math.Cos(w0);
            double alpha = Math.Sin(w0) / (2 * q);
            double a0 = 1 + alpha;
            b0 = (1 - cw) * 0.5 / a0;
            b1 = (1 - cw) / a0;
            b2 = b0;
            a1 = (-2 * cw) / a0;
            a2 = (1 - alpha) / a0;
            kind = "lp"; lastFc = fc; lastQ = q;
        }

        /// <summary>高通：砍掉低频，用于去直流与“电话音”。</summary>
        public void SetHighPass(double fc, double q)
        {
            fc = Math.Clamp(fc, 10.0, XuConst.SampleRate * 0.49);
            q = Math.Clamp(q, 0.1, 20.0);
            if (kind == "hp" && Math.Abs(fc - lastFc) < 0.5 && Math.Abs(q - lastQ) < 1e-3) return;
            double w0 = 2 * Math.PI * fc / XuConst.SampleRate;
            double cw = Math.Cos(w0);
            double alpha = Math.Sin(w0) / (2 * q);
            double a0 = 1 + alpha;
            b0 = (1 + cw) * 0.5 / a0;
            b1 = -(1 + cw) / a0;
            b2 = b0;
            a1 = (-2 * cw) / a0;
            a2 = (1 - alpha) / a0;
            kind = "hp"; lastFc = fc; lastQ = q;
        }

        /// <summary>常量峰值增益带通。</summary>
        public void SetBandPass(double fc, double q)
        {
            fc = Math.Clamp(fc, 20.0, XuConst.SampleRate * 0.49);
            q = Math.Clamp(q, 0.1, 30.0);
            if (kind == "bp" && Math.Abs(fc - lastFc) < 0.5 && Math.Abs(q - lastQ) < 1e-3) return;
            double w0 = 2 * Math.PI * fc / XuConst.SampleRate;
            double cw = Math.Cos(w0);
            double alpha = Math.Sin(w0) / (2 * q);
            double a0 = 1 + alpha;
            b0 = alpha / a0;
            b1 = 0;
            b2 = -alpha / a0;
            a1 = (-2 * cw) / a0;
            a2 = (1 - alpha) / a0;
            kind = "bp"; lastFc = fc; lastQ = q;
        }

        /// <summary>对一个样本执行差分方程；实时路径调用，不能分配。</summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public double Process(double x)
        {
            // y = b0*x + z1
            double y = b0 * x + z1;
            // 更新两条状态：这是 DF2T 的标准写法，数值稳定性最好
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        /// <summary>触发新声音前清空状态，避免上一槌的残余造成“噗”。</summary>
        public void Reset()
        {
            z1 = 0; z2 = 0;
        }
    }

    /// <summary>单声部用户参数（0..1，除 level/pan 外都按规格书映射）。</summary>
    public sealed class XuPadParams
    {
        public float Tune = 0.50f;
        public float Decay = 0.50f;
        public float Tone = 0.50f;
        public float Snappy = 0.55f;
        public float Tail = 0.45f;
        public float Level = 0.75f;
        public float Pan = 0.50f;
        public bool Mute;
        public bool Solo;
    }

    /// <summary>
    /// 一个鼓槌（voice）。所有包络都由“年龄 t”解析计算。
    /// </summary>
    public sealed class XuVoice
    {
        public int Pad;                 // 所属声部
        public bool Active;             // 是否在发声
        public long Start;              // 触发绝对样本
        public long Stop = long.MaxValue; // 强制结束时刻
        public double Gain = 1.0;       // 力度增益（含重音）
        public bool Accent;             // 重音标志
        public int FadeLen = 64;        // 掐断淡出长度

        private readonly float[] noise; // 预生成噪声池
        private int npos;               // 噪声读取指针
        private readonly XuBiquad bp = new XuBiquad();
        private readonly XuBiquad hp = new XuBiquad();
        private readonly XuBiquad bp2 = new XuBiquad();

        public XuVoice(int pad, int seed)
        {
            Pad = pad;
            // 8192 个噪声样本 = 32KB；用固定种子保证每次运行音色一致
            noise = new float[8192];
            var rng = new Random(seed + pad * 7919);
            for (int i = 0; i < noise.Length; i++) noise[i] = (float)(rng.NextDouble() * 2 - 1);
        }

        /// <summary>触发：重置状态并记录起点。</summary>
        public void Trigger(long when, double gain, bool accent)
        {
            Active = true;
            Start = when;
            Stop = long.MaxValue;
            Gain = gain;
            Accent = accent;
            npos = 0;
            bp.Reset(); hp.Reset(); bp2.Reset();
        }

        /// <summary>掐断：在 when 之后做 fade 样本淡出。</summary>
        public void Kill(long when, int fade = 24)
        {
            if (!Active) return;
            // stop=when+fade：淡出斜坡发生在 stop 之前的 fade 个样本，
            // 若直接 stop=when，未来样本全被裁掉，仍然是硬切。
            Stop = Math.Min(Stop, when + fade);
            FadeLen = Math.Max(1, fade);
        }

        /// <summary>循环取噪声，避免在音频路径里 new 数组。</summary>
        private float Noise()
        {
            if (npos >= noise.Length) npos = 0;
            return noise[npos++];
        }

        /// <summary>
        /// 把 [b0,b1) 区间叠加进左右声道。
        /// 注意：这里用 double 计算，最后才转 float；48k 下浮点余量充足。
        /// </summary>
        public void Render(float[] left, float[] right, int offset, int frames,
                           long b0, long b1, XuPadParams p)
        {
            if (!Active) return;
            long a = Math.Max(b0, Start);
            long b = Math.Min(b1, Stop);
            if (b <= a)
            {
                if (b1 > Stop) Active = false;
                return;
            }
            int m = (int)(b - a);
            int idx = (int)(a - b0) + offset;

            // 等功率声像：pan=0.5 时左右各 cos/sin(π/4)=0.707
            double pan = Math.Clamp(p.Pan, 0f, 1f);
            double lg = Math.Cos(pan * Math.PI * 0.5);
            double rg = Math.Sin(pan * Math.PI * 0.5);
            double g = Gain * Math.Max(p.Level, 0f) * 0.42;

            for (int i = 0; i < m; i++)
            {
                double t = (a + i - Start) / (double)XuConst.SampleRate;
                double s = Synth(t, p);
                // 掐断淡出：距离 Stop 越近增益越小
                if (Stop < long.MaxValue / 2)
                {
                    long dist = Stop - (a + i);
                    if (dist < FadeLen) s *= dist < 0 ? 0 : (double)dist / FadeLen;
                }
                left[idx + i] += (float)(s * g * lg);
                right[idx + i] += (float)(s * g * rg);
            }
            if (b1 - Start > 4L * XuConst.SampleRate) Active = false;
        }

        /// <summary>16 种声部的合成分派。</summary>
        private double Synth(double t, XuPadParams p)
        {
            int pad = Pad;
            // ---------------- 0 底鼓 ----------------
            if (pad == 0)
            {
                double fEnd = 40 + p.Tune * 40;
                double fStart = fEnd * (1.6 + p.Tone * 1.9);
                double tau = 0.018 + (1 - p.Decay) * 0.012;
                double df = fStart - fEnd;
                double phase = 2 * Math.PI * (fEnd * t + df * tau * (1 - Math.Exp(-t / tau)));
                double dec = (0.12 + p.Decay * 1.48) * (Accent ? 1.18 : 1.0);
                double amp = Math.Exp(-t / dec);
                double n = Noise();
                hp.SetHighPass(1800 + p.Tone * 3000, 0.8);
                double click = hp.Process(n) * Math.Exp(-t / 0.0035) * (0.06 + p.Tone * 0.32);
                double body = Math.Tanh(Math.Sin(phase) * (1.15 + p.Decay * 1.2)) * 0.92;
                return body * amp + click;
            }
            // ---------------- 1 军鼓 ----------------
            if (pad == 1)
            {
                double f1 = 150 + p.Tune * 110;
                double f2 = f1 * (1.78 + p.Tone * 0.22);
                double decO = 0.055 + p.Decay * 0.14;
                double osc = 0.55 * Math.Sin(2 * Math.PI * f1 * t) * Math.Exp(-t / decO)
                           + 0.34 * Math.Sin(2 * Math.PI * f2 * t) * Math.Exp(-t / (decO * 0.72));
                double n = Noise();
                bp.SetBandPass(1200 + p.Snappy * 3800, 0.9 + p.Tone * 0.8);
                double nz = bp.Process(n);
                double decN = (0.075 + p.Decay * 0.16) * (Accent ? 0.85 : 1.0);
                nz *= Math.Exp(-t / decN) * (0.25 + p.Snappy * 0.7);
                return osc * 0.7 + nz;
            }
            // ---------------- 2..7 嗵鼓 / 康加 ----------------
            if (pad >= 2 && pad <= 7)
            {
                double baseF; double decayS; double clickAmt; double sweep;
                if (pad <= 4)
                {
                    baseF = pad == 2 ? 80 : pad == 3 ? 120 : 170;
                    decayS = 0.15 + p.Decay * 0.75;
                    clickAmt = 0.04 + p.Tone * 0.22;
                    sweep = 1.9;
                }
                else
                {
                    baseF = pad == 5 ? 220 : pad == 6 ? 300 : 400;
                    decayS = 0.10 + p.Decay * 0.55;
                    clickAmt = 0.02 + p.Tone * 0.12;
                    sweep = 1.35;
                }
                double f0 = baseF * (0.6 + p.Tune);
                double tau = pad <= 4 ? 0.035 : 0.02;
                double phase = 2 * Math.PI * (f0 * t + f0 * (sweep - 1) * tau * (1 - Math.Exp(-t / tau)));
                double amp = Math.Exp(-t / decayS);
                double n = Noise();
                hp.SetHighPass(2500, 0.7);
                double click = hp.Process(n) * Math.Exp(-t / 0.004) * clickAmt;
                return Math.Sin(phase) * amp * 0.9 + click;
            }
            // ---------------- 8 边击 ----------------
            if (pad == 8)
            {
                double f = 1200 + p.Tune * 1400;
                double dec = 0.012 + p.Decay * 0.05;
                double sq = Math.Sin(2 * Math.PI * f * t) >= 0 ? 1 : -1;
                bp.SetBandPass(f, 2.2);
                double body = bp.Process(sq) * Math.Exp(-t / dec);
                double n = Noise();
                hp.SetHighPass(3000, 0.8);
                double tick = hp.Process(n) * Math.Exp(-t / 0.004) * 0.35;
                return body * 0.8 + tick;
            }
            // ---------------- 9 击木 ----------------
            if (pad == 9)
            {
                double f = 2000 + p.Tune * 1000;
                double dec = 0.018 + p.Decay * 0.035;
                return Math.Sin(2 * Math.PI * f * t) * Math.Exp(-t / dec) * 0.9;
            }
            // ---------------- 10 拍手 ----------------
            if (pad == 10)
            {
                double n = Noise();
                bp.SetBandPass(800 + p.Tone * 1700, 1.1);
                double nz = bp.Process(n);
                double env = 0;
                for (int k = 0; k < 4; k++)
                {
                    double tk = k * 0.009;
                    if (t >= tk) env += Math.Exp(-(t - tk) / 0.011) * 0.55;
                }
                double tail = Math.Exp(-t / (0.05 + p.Decay * 0.35)) * (0.15 + p.Tail * 0.55);
                return nz * (env + tail);
            }
            // ---------------- 11 沙锤 ----------------
            if (pad == 11)
            {
                double n = Noise();
                hp.SetHighPass(4000 + p.Tone * 5000, 0.9);
                double nz = hp.Process(n);
                double env = (1 - Math.Exp(-t / 0.0015)) * Math.Exp(-t / (0.018 + p.Decay * 0.12));
                return nz * env * 0.85;
            }
            // ---------------- 12 牛铃 ----------------
            if (pad == 12)
            {
                double f = 400 + p.Tune * 300;
                double sq1 = Math.Sin(2 * Math.PI * f * t) >= 0 ? 1 : -1;
                double sq2 = Math.Sin(2 * Math.PI * f * 1.48 * t) >= 0 ? 1 : -1;
                bp.SetBandPass(2400 + p.Tone * 1200, 1.4);
                double body = bp.Process(sq1 * 0.5 + sq2 * 0.5);
                double dec = 0.05 + p.Decay * 0.35;
                return body * Math.Exp(-t / dec) * 0.85;
            }
            // ---------------- 13..15 镲族 ----------------
            double[] ratios = { 1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523 };
            double root = 320 * (0.75 + p.Tune * 0.5);
            double metal = 0;
            foreach (double r in ratios) metal += Math.Sin(2 * Math.PI * root * r * t) >= 0 ? 1 : -1;
            metal /= ratios.Length;
            hp.SetHighPass(5200 + p.Tone * 2600, 0.8);
            metal = hp.Process(metal);
            bp.SetBandPass(7600 + p.Tone * 3600, 0.9);
            metal = bp.Process(metal);
            double decH = pad == 15 ? 0.028 + p.Decay * 0.20
                        : pad == 14 ? 0.12 + p.Decay * 0.65
                                    : 0.25 + p.Decay * 1.75;
            double envH = Math.Exp(-t / decH);
            double outp = metal * envH;
            if (pad == 13)
            {
                double n = Noise();
                bp2.SetBandPass(10000, 0.7);
                outp += bp2.Process(n) * envH * 0.12;
            }
            return outp * 0.75;
        }
    }
}
