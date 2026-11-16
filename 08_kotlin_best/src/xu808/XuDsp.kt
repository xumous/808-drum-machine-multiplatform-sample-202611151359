// ============================================================================
//  XuDsp.kt —— 十六声部模拟合成（Kotlin 版）  ·  徐某
//
//  与 Java 版逐行等价：DSP 公式一个都不改，保证两个版本音色一致。
//  音频路径“零分配”：
//    · 噪声表、滤波器状态全部在构造时创建；
//    · 渲染只做标量运算，不 new 任何对象；
//    · 包络是时间 t 的解析函数，不需要逐样本状态机。
// ============================================================================
package xu808

import java.util.Random
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.sin
import kotlin.math.tanh

object XuDsp {

    /** 采样率（与工程一致）。 */
    private const val SR: Double = 48000.0
    private const val TWO_PI = PI * 2.0

    fun clamp(x: Double, lo: Double, hi: Double): Double = if (x < lo) lo else if (x > hi) hi else x

    // ------------------------------------------------------------------
    // RBJ 双二阶滤波器：低通 / 高通 / 带通
    // 系数用“当前频率与 Q”缓存，避免每样本重复算 sin/cos。
    // ------------------------------------------------------------------
    class Biquad {
        private var b0 = 1.0
        private var b1 = 0.0
        private var b2 = 0.0
        private var a1 = 0.0
        private var a2 = 0.0
        private var z1 = 0.0
        private var z2 = 0.0
        private var kind = 0
        private var fc = 0.0
        private var q = 1.0

        fun setLowPass(f: Double, qq: Double) {
            val f2 = clamp(f, 20.0, SR * 0.49)
            val q2 = clamp(qq, 0.1, 20.0)
            if (kind == 1 && abs(f2 - fc) < 0.5 && abs(q2 - q) < 1e-3) return
            val w0 = TWO_PI * f2 / SR
            val cw = cos(w0)
            val alpha = sin(w0) / (2 * q2)
            val a0 = 1 + alpha
            b0 = (1 - cw) * 0.5 / a0
            b1 = (1 - cw) / a0
            b2 = b0
            a1 = -2 * cw / a0
            a2 = (1 - alpha) / a0
            kind = 1; fc = f2; q = q2
        }

        fun setHighPass(f: Double, qq: Double) {
            val f2 = clamp(f, 10.0, SR * 0.49)
            val q2 = clamp(qq, 0.1, 20.0)
            if (kind == 2 && abs(f2 - fc) < 0.5 && abs(q2 - q) < 1e-3) return
            val w0 = TWO_PI * f2 / SR
            val cw = cos(w0)
            val alpha = sin(w0) / (2 * q2)
            val a0 = 1 + alpha
            b0 = (1 + cw) * 0.5 / a0
            b1 = -(1 + cw) / a0
            b2 = b0
            a1 = -2 * cw / a0
            a2 = (1 - alpha) / a0
            kind = 2; fc = f2; q = q2
        }

        fun setBandPass(f: Double, qq: Double) {
            val f2 = clamp(f, 20.0, SR * 0.49)
            val q2 = clamp(qq, 0.1, 30.0)
            if (kind == 3 && abs(f2 - fc) < 0.5 && abs(q2 - q) < 1e-3) return
            val w0 = TWO_PI * f2 / SR
            val cw = cos(w0)
            val alpha = sin(w0) / (2 * q2)
            val a0 = 1 + alpha
            b0 = alpha / a0
            b1 = 0.0
            b2 = -alpha / a0
            a1 = -2 * cw / a0
            a2 = (1 - alpha) / a0
            kind = 3; fc = f2; q = q2
        }

        fun run(x: Double): Double {
            val y = b0 * x + z1
            z1 = b1 * x - a1 * y + z2
            z2 = b2 * x - a2 * y
            return y
        }

        fun reset() {
            z1 = 0.0; z2 = 0.0
        }
    }

    // ------------------------------------------------------------------
    // 一个 voice：4 个并发，超过则抢占最老的（见 XuEngine.trigger）
    // ------------------------------------------------------------------
    class Voice(val pad: Int) {
        var active = false
        var start = 0L
        var stop = Long.MAX_VALUE
        private var gain = 1.0
        private var accent = false
        private var fadeLen = 64

        // 固定噪声表：同一 voice 反复触发也用同一段噪声，省掉运行时随机数开销
        private val noiseTable = FloatArray(8192)
        private var npos = 0
        private val bp = Biquad()
        private val hp = Biquad()
        private val bp2 = Biquad()

        init {
            // 每个声部用固定种子，保证导出与实时的噪声完全一致（可重复）
            val r = Random(0x808L + pad * 7919L)
            for (i in noiseTable.indices) noiseTable[i] = (r.nextDouble() * 2 - 1).toFloat()
        }

        fun trigger(whenT: Long, g: Double, acc: Boolean) {
            active = true
            start = whenT
            stop = Long.MAX_VALUE
            gain = g
            accent = acc
            fadeLen = 64
            npos = 0
            bp.reset(); hp.reset(); bp2.reset()
        }

        /** 掐断（用于 choke 与 voice stealing），fade 为线性淡出样本数。 */
        fun kill(whenT: Long, fade: Int) {
            if (!active) return
            if (whenT < stop) stop = whenT
            fadeLen = Math.max(1, fade)
        }

        private fun noise(): Double {
            if (npos >= noiseTable.size) npos = 0
            return noiseTable[npos++].toDouble()
        }

        private fun square(phase: Double) = if (sin(phase) >= 0) 1.0 else -1.0

        /**
         * 把本 voice 叠加到左右声道。
         * L/R 以 b0 为原点；只渲染 [a,b) 这一段，实现“事件在样本级落位”。
         */
        fun render(L: FloatArray, R: FloatArray, n: Int, b0: Long, b1: Long, p: XuProject.Pad) {
            if (!active) return
            val a = Math.max(b0, start)
            val b = Math.min(b1, stop)
            if (b <= a) {
                if (b1 > stop) active = false
                return
            }
            val m = (b - a).toInt()
            val off = (a - b0).toInt()
            if (m <= 0 || off >= n) return
            val pan = clamp(p.pan.toDouble(), 0.0, 1.0)
            // 等功率声像：中间时左右各 -3dB
            val lg = cos(pan * PI * 0.5)
            val rg = sin(pan * PI * 0.5)
            val g = gain * Math.max(0.0, p.level) * 0.42
            for (i in 0 until m) {
                val absSample = a + i
                val t = (absSample - start).toDouble() / SR
                var s = synth(t, p)
                if (stop < Long.MAX_VALUE / 2) {
                    val dist = stop - absSample
                    if (dist < fadeLen) s *= if (dist <= 0) 0.0 else dist.toDouble() / fadeLen
                }
                L[off + i] += (s * g * lg).toFloat()
                R[off + i] += (s * g * rg).toFloat()
            }
            // 4 秒还没结束的 voice 强制回收，杜绝“采样循环残留”
            if (b1 - start > 4L * XuProject.SR) active = false
        }

        // ------------------------------------------------------------------
        // 各声部合成公式（与 Java 版完全一致）
        // ------------------------------------------------------------------
        private fun synth(t: Double, p: XuProject.Pad): Double {
            when (pad) {
                // 0 BD：桥接 T 型正弦。相位用“频率积分”的解析式，
                // 因为音高从 fStart 指数滑到 fEnd，直接 sin(2πft) 会有爆音。
                0 -> {
                    val fEnd = 40 + p.tune * 40.0
                    val fStart = fEnd * (1.6 + p.tone * 1.9)
                    val tau = 0.018 + (1 - p.decay) * 0.012
                    val df = fStart - fEnd
                    val phase = TWO_PI * (fEnd * t + df * tau * (1 - exp(-t / tau)))
                    val dec = (0.12 + p.decay * 1.48) * (if (accent) 1.18 else 1.0)
                    val amp = exp(-t / dec)
                    val nz = noise()
                    hp.setHighPass(1800 + p.tone * 3000.0, 0.8)
                    val click = hp.run(nz) * exp(-t / 0.0035) * (0.06 + p.tone * 0.32)
                    val body = tanh(sin(phase) * (1.15 + p.decay * 1.2)) * 0.92
                    return body * amp + click
                }
                // 1 SD：180/330Hz 双正弦 + 带通噪声
                1 -> {
                    val f1 = 150 + p.tune * 110.0
                    val f2 = f1 * (1.78 + p.tone * 0.22)
                    val decO = 0.055 + p.decay * 0.14
                    val osc = 0.55 * sin(TWO_PI * f1 * t) * exp(-t / decO) +
                            0.34 * sin(TWO_PI * f2 * t) * exp(-t / (decO * 0.72))
                    val nz = noise()
                    bp.setBandPass(1200 + p.snappy * 3800.0, 0.9 + p.tone * 0.8)
                    var noiseOut = bp.run(nz)
                    val decN = (0.075 + p.decay * 0.16) * (if (accent) 0.85 else 1.0)
                    noiseOut *= exp(-t / decN) * (0.25 + p.snappy * 0.7)
                    return osc * 0.7 + noiseOut
                }
                // 2..7：嗵鼓 / 康加，同一种“正弦下滑 + 点击”模型，参数不同
                in 2..7 -> {
                    val baseF: Double
                    val decayS: Double
                    val clickAmt: Double
                    val sweep: Double
                    if (pad <= 4) {
                        baseF = if (pad == 2) 80.0 else if (pad == 3) 120.0 else 170.0
                        decayS = 0.15 + p.decay * 0.75
                        clickAmt = 0.04 + p.tone * 0.22
                        sweep = 1.9
                    } else {
                        baseF = if (pad == 5) 220.0 else if (pad == 6) 300.0 else 400.0
                        decayS = 0.10 + p.decay * 0.55
                        clickAmt = 0.02 + p.tone * 0.12
                        sweep = 1.35
                    }
                    val f0 = baseF * (0.6 + p.tune)
                    val tau = if (pad <= 4) 0.035 else 0.02
                    val phase = TWO_PI * (f0 * t + f0 * (sweep - 1) * tau * (1 - exp(-t / tau)))
                    val amp = exp(-t / decayS)
                    val nz = noise()
                    hp.setHighPass(2500.0, 0.7)
                    val click = hp.run(nz) * exp(-t / 0.004) * clickAmt
                    return sin(phase) * amp * 0.9 + click
                }
                // 8 RS 边击：1.7k 方波脉冲 + 短噪声
                8 -> {
                    val f = 1200 + p.tune * 1400.0
                    val dec = 0.012 + p.decay * 0.05
                    val sq = square(TWO_PI * f * t)
                    bp.setBandPass(f, 2.2)
                    val body = bp.run(sq) * exp(-t / dec)
                    val nz = noise()
                    hp.setHighPass(3000.0, 0.8)
                    val tick = hp.run(nz) * exp(-t / 0.004) * 0.35
                    return body * 0.8 + tick
                }
                // 9 CL 击木：2.5kHz 正弦，30ms
                9 -> {
                    val f = 2000 + p.tune * 1000.0
                    val dec = 0.018 + p.decay * 0.035
                    return sin(TWO_PI * f * t) * exp(-t / dec) * 0.9
                }
                // 10 CP 拍手：4 段 9ms 间隔的噪声爆发 + 尾巴
                10 -> {
                    val nz = noise()
                    bp.setBandPass(800 + p.tone * 1700.0, 1.1)
                    val filt = bp.run(nz)
                    var env = 0.0
                    for (k in 0 until 4) {
                        val tk = k * 0.009
                        if (t >= tk) env += exp(-(t - tk) / 0.011) * 0.55
                    }
                    val tail = exp(-t / (0.05 + p.decay * 0.35)) * (0.15 + p.tail * 0.55)
                    return filt * (env + tail)
                }
                // 11 MA 沙锤：高通噪声，40ms
                11 -> {
                    val nz = noise()
                    hp.setHighPass(4000 + p.tone * 5000.0, 0.9)
                    val filt = hp.run(nz)
                    val env = (1 - exp(-t / 0.0015)) * exp(-t / (0.018 + p.decay * 0.12))
                    return filt * env * 0.85
                }
                // 12 CB 牛铃：540/800Hz 双方波 + 带通
                12 -> {
                    val f = 400 + p.tune * 300.0
                    val sq1 = square(TWO_PI * f * t)
                    val sq2 = square(TWO_PI * f * 1.48 * t)
                    bp.setBandPass(2400 + p.tone * 1200.0, 1.4)
                    val body = bp.run(sq1 * 0.5 + sq2 * 0.5)
                    val dec = 0.05 + p.decay * 0.35
                    return body * exp(-t / dec) * 0.85
                }
                // 13..15 CY/OH/CH：6 个非谐方波金属簇 + 高通 + 带通
                else -> {
                    val ratios = doubleArrayOf(1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523)
                    val root = 320 * (0.75 + p.tune * 0.5)
                    var metal = 0.0
                    for (r in ratios) metal += square(TWO_PI * root * r * t)
                    metal /= ratios.size
                    hp.setHighPass(5200 + p.tone * 2600.0, 0.8)
                    metal = hp.run(metal)
                    bp.setBandPass(7600 + p.tone * 3600.0, 0.9)
                    metal = bp.run(metal)
                    val dec = when (pad) {
                        15 -> 0.028 + p.decay * 0.20
                        14 -> 0.12 + p.decay * 0.65
                        else -> 0.25 + p.decay * 1.75
                    }
                    val env = exp(-t / dec)
                    var out = metal * env
                    if (pad == 13) {
                        // 吊镲额外叠一层 10kHz 噪声，增加“沙”感
                        val nz = noise()
                        bp2.setBandPass(10000.0, 0.7)
                        out += bp2.run(nz) * env * 0.12
                    }
                    return out * 0.75
                }
            }
        }
    }
}
