// ============================================================================
//  XuDsp.java —— 十六声部模拟合成（Java 版）
//
//  Java 的 GC 与实时音频天生有矛盾，所以徐某把音频路径写成“零分配”：
//    · 噪声池、滤波器状态都在构造时创建；
//    · 渲染只做标量运算，不 new 任何对象；
//    · 包络是时间 t 的解析函数，不需要逐样本状态机。
//  这样即使发生一次 Young GC，回调里也没有可回收的临时对象。
// ============================================================================
package xu808;

import java.util.Random;

public final class XuDsp {
    public static final double PI = 3.14159265358979323846;
    public static final double SR = XuProject.SR;

    private XuDsp() {
    }

    public static double clamp(double x, double lo, double hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    // ------------------------------------------------------------------
    // RBJ 双二阶
    // ------------------------------------------------------------------
    public static final class Biquad {
        private double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        private int kind = 0;
        private double fc = 0, q = 1;

        public void setLowPass(double f, double qq) {
            f = clamp(f, 20, SR * 0.49);
            qq = clamp(qq, 0.1, 20);
            if (kind == 1 && Math.abs(f - fc) < 0.5 && Math.abs(qq - q) < 1e-3) return;
            double w0 = 2 * PI * f / SR, cw = Math.cos(w0);
            double alpha = Math.sin(w0) / (2 * qq), a0 = 1 + alpha;
            b0 = (1 - cw) * 0.5 / a0;
            b1 = (1 - cw) / a0;
            b2 = b0;
            a1 = -2 * cw / a0;
            a2 = (1 - alpha) / a0;
            kind = 1;
            fc = f;
            q = qq;
        }

        public void setHighPass(double f, double qq) {
            f = clamp(f, 10, SR * 0.49);
            qq = clamp(qq, 0.1, 20);
            if (kind == 2 && Math.abs(f - fc) < 0.5 && Math.abs(qq - q) < 1e-3) return;
            double w0 = 2 * PI * f / SR, cw = Math.cos(w0);
            double alpha = Math.sin(w0) / (2 * qq), a0 = 1 + alpha;
            b0 = (1 + cw) * 0.5 / a0;
            b1 = -(1 + cw) / a0;
            b2 = b0;
            a1 = -2 * cw / a0;
            a2 = (1 - alpha) / a0;
            kind = 2;
            fc = f;
            q = qq;
        }

        public void setBandPass(double f, double qq) {
            f = clamp(f, 20, SR * 0.49);
            qq = clamp(qq, 0.1, 30);
            if (kind == 3 && Math.abs(f - fc) < 0.5 && Math.abs(qq - q) < 1e-3) return;
            double w0 = 2 * PI * f / SR, cw = Math.cos(w0);
            double alpha = Math.sin(w0) / (2 * qq), a0 = 1 + alpha;
            b0 = alpha / a0;
            b1 = 0;
            b2 = -alpha / a0;
            a1 = -2 * cw / a0;
            a2 = (1 - alpha) / a0;
            kind = 3;
            fc = f;
            q = qq;
        }

        public double run(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        public void reset() {
            z1 = 0;
            z2 = 0;
        }
    }

    // ------------------------------------------------------------------
    // 一个 voice
    // ------------------------------------------------------------------
    public static final class Voice {
        public final int pad;
        private final float[] noise = new float[8192];
        private final Biquad bp = new Biquad(), hp = new Biquad(), bp2 = new Biquad();
        public boolean active = false;
        public long start = 0;
        public long stop = Long.MAX_VALUE;
        private float gain = 1;
        private boolean accent = false;
        private int fadeLen = 64;
        private int npos = 0;

        public Voice(int pad) {
            this.pad = pad;
            Random r = new Random(0x808 + pad * 7919L);
            for (int i = 0; i < noise.length; i++) noise[i] = (float) (r.nextDouble() * 2 - 1);
        }

        private static double square(double phase) {
            return Math.sin(phase) >= 0 ? 1 : -1;
        }

        public void trigger(long when, float g, boolean acc) {
            active = true;
            start = when;
            stop = Long.MAX_VALUE;
            gain = g;
            accent = acc;
            fadeLen = 64;
            npos = 0;
            bp.reset();
            hp.reset();
            bp2.reset();
        }

        public void kill(long when, int fade) {
            if (!active) return;
            stop = Math.min(stop, when);
            fadeLen = Math.max(1, fade);
        }

        private double noise() {
            if (npos >= noise.length) npos = 0;
            return noise[npos++];
        }

        /**
         * 把所有声部叠加到左右声道；索引以 b0 为原点。
         */
        public void render(float[] L, float[] R, int n, long b0, long b1,
                           XuProject.Pad p) {
            if (!active) return;
            long a = Math.max(b0, start);
            long b = Math.min(b1, stop);
            if (b <= a) {
                if (b1 > stop) active = false;
                return;
            }
            int m = (int) (b - a);
            int off = (int) (a - b0);
            if (m <= 0 || off >= n) return;
            double pan = clamp(p.pan, 0, 1);
            double lg = Math.cos(pan * PI * 0.5), rg = Math.sin(pan * PI * 0.5);
            double g = gain * Math.max(0f, p.level) * 0.42;
            for (int i = 0; i < m; i++) {
                long abs = a + i;
                double t = (double) (abs - start) / SR;
                double s = synth(t, p);
                if (stop < Long.MAX_VALUE / 2) {
                    long dist = stop - abs;
                    if (dist < fadeLen) s *= dist <= 0 ? 0 : (double) dist / fadeLen;
                }
                L[off + i] += (float) (s * g * lg);
                R[off + i] += (float) (s * g * rg);
            }
            if (b1 - start > 4L * XuProject.SR) active = false;
        }

        private double synth(double t, XuProject.Pad p) {
            switch (pad) {
                case 0: {
                    double fEnd = 40 + p.tune * 40;
                    double fStart = fEnd * (1.6 + p.tone * 1.9);
                    double tau = 0.018 + (1 - p.decay) * 0.012;
                    double df = fStart - fEnd;
                    double phase = 2 * PI * (fEnd * t + df * tau * (1 - Math.exp(-t / tau)));
                    double dec = (0.12 + p.decay * 1.48) * (accent ? 1.18 : 1);
                    double amp = Math.exp(-t / dec);
                    double n = noise();
                    hp.setHighPass(1800 + p.tone * 3000, 0.8);
                    double click = hp.run(n) * Math.exp(-t / 0.0035) * (0.06 + p.tone * 0.32);
                    double body = Math.tanh(Math.sin(phase) * (1.15 + p.decay * 1.2)) * 0.92;
                    return body * amp + click;
                }
                case 1: {
                    double f1 = 150 + p.tune * 110;
                    double f2 = f1 * (1.78 + p.tone * 0.22);
                    double decO = 0.055 + p.decay * 0.14;
                    double osc = 0.55 * Math.sin(2 * PI * f1 * t) * Math.exp(-t / decO)
                            + 0.34 * Math.sin(2 * PI * f2 * t) * Math.exp(-t / (decO * 0.72));
                    double n = noise();
                    bp.setBandPass(1200 + p.snappy * 3800, 0.9 + p.tone * 0.8);
                    double nz = bp.run(n);
                    double decN = (0.075 + p.decay * 0.16) * (accent ? 0.85 : 1);
                    nz *= Math.exp(-t / decN) * (0.25 + p.snappy * 0.7);
                    return osc * 0.7 + nz;
                }
                case 2:
                case 3:
                case 4:
                case 5:
                case 6:
                case 7: {
                    double baseF, decayS, clickAmt, sweep;
                    if (pad <= 4) {
                        baseF = pad == 2 ? 80 : pad == 3 ? 120 : 170;
                        decayS = 0.15 + p.decay * 0.75;
                        clickAmt = 0.04 + p.tone * 0.22;
                        sweep = 1.9;
                    } else {
                        baseF = pad == 5 ? 220 : pad == 6 ? 300 : 400;
                        decayS = 0.10 + p.decay * 0.55;
                        clickAmt = 0.02 + p.tone * 0.12;
                        sweep = 1.35;
                    }
                    double f0 = baseF * (0.6 + p.tune);
                    double tau = pad <= 4 ? 0.035 : 0.02;
                    double phase = 2 * PI * (f0 * t + f0 * (sweep - 1) * tau * (1 - Math.exp(-t / tau)));
                    double amp = Math.exp(-t / decayS);
                    double n = noise();
                    hp.setHighPass(2500, 0.7);
                    double click = hp.run(n) * Math.exp(-t / 0.004) * clickAmt;
                    return Math.sin(phase) * amp * 0.9 + click;
                }
                case 8: {
                    double f = 1200 + p.tune * 1400;
                    double dec = 0.012 + p.decay * 0.05;
                    double sq = square(2 * PI * f * t);
                    bp.setBandPass(f, 2.2);
                    double body = bp.run(sq) * Math.exp(-t / dec);
                    double n = noise();
                    hp.setHighPass(3000, 0.8);
                    double tick = hp.run(n) * Math.exp(-t / 0.004) * 0.35;
                    return body * 0.8 + tick;
                }
                case 9: {
                    double f = 2000 + p.tune * 1000;
                    double dec = 0.018 + p.decay * 0.035;
                    return Math.sin(2 * PI * f * t) * Math.exp(-t / dec) * 0.9;
                }
                case 10: {
                    double n = noise();
                    bp.setBandPass(800 + p.tone * 1700, 1.1);
                    double nz = bp.run(n);
                    double env = 0;
                    for (int k = 0; k < 4; k++) {
                        double tk = k * 0.009;
                        if (t >= tk) env += Math.exp(-(t - tk) / 0.011) * 0.55;
                    }
                    double tail = Math.exp(-t / (0.05 + p.decay * 0.35)) * (0.15 + p.tail * 0.55);
                    return nz * (env + tail);
                }
                case 11: {
                    double n = noise();
                    hp.setHighPass(4000 + p.tone * 5000, 0.9);
                    double nz = hp.run(n);
                    double env = (1 - Math.exp(-t / 0.0015)) * Math.exp(-t / (0.018 + p.decay * 0.12));
                    return nz * env * 0.85;
                }
                case 12: {
                    double f = 400 + p.tune * 300;
                    double sq1 = square(2 * PI * f * t);
                    double sq2 = square(2 * PI * f * 1.48 * t);
                    bp.setBandPass(2400 + p.tone * 1200, 1.4);
                    double body = bp.run(sq1 * 0.5 + sq2 * 0.5);
                    double dec = 0.05 + p.decay * 0.35;
                    return body * Math.exp(-t / dec) * 0.85;
                }
                default: {
                    double[] ratios = {1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523};
                    double root = 320 * (0.75 + p.tune * 0.5);
                    double metal = 0;
                    for (double r : ratios) metal += square(2 * PI * root * r * t);
                    metal /= ratios.length;
                    hp.setHighPass(5200 + p.tone * 2600, 0.8);
                    metal = hp.run(metal);
                    bp.setBandPass(7600 + p.tone * 3600, 0.9);
                    metal = bp.run(metal);
                    double dec = pad == 15 ? 0.028 + p.decay * 0.20
                            : pad == 14 ? 0.12 + p.decay * 0.65
                            : 0.25 + p.decay * 1.75;
                    double env = Math.exp(-t / dec);
                    double out = metal * env;
                    if (pad == 13) {
                        double n = noise();
                        bp2.setBandPass(10000, 0.7);
                        out += bp2.run(n) * env * 0.12;
                    }
                    return out * 0.75;
                }
            }
        }
    }
}
