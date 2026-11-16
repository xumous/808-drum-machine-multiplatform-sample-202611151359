// ============================================================================
//  XuEngine.kt —— 音序器、混音与导出（Kotlin 版）  ·  徐某
//
//  音频路径依旧“零分配”，与 Java 版一一对应：
//    · 事件不是对象，而是四条平行数组（时刻/声部/力度/重音）；
//    · 队列容量固定 4096，满了丢最旧的，绝不扩容；
//    · 渲染只用预分配的 FloatArray。
//  时钟完全来自音频回调累计的 samplePos，UI 定时器不参与任何时序。
// ============================================================================
package xu808

import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.FileOutputStream
import java.nio.charset.StandardCharsets
import java.util.Arrays
import java.util.Comparator

class XuEngine(var proj: XuProject) {

    companion object {
        const val BLOCK = 512               // 512 @48k ≈ 10.7ms
        private const val MAX_EVENTS = 4096 // 事件队列硬上限

        // ------------------------------------------------------------------
        // WAV 写出：16bit / 48kHz / 立体声 PCM
        // ------------------------------------------------------------------
        fun writeWav(path: String, interleaved: FloatArray) {
            val frames = interleaved.size / 2
            DataOutputStream(BufferedOutputStream(FileOutputStream(path), 1 shl 16)).use { os ->
                writeFourCC(os, "RIFF")
                writeLE32(os, 36 + frames * 4)
                writeFourCC(os, "WAVE")
                writeFourCC(os, "fmt ")
                writeLE32(os, 16)
                writeLE16(os, 1)                  // PCM
                writeLE16(os, 2)                  // 2 声道
                writeLE32(os, XuProject.SR)
                writeLE32(os, XuProject.SR * 4)   // 每秒字节数
                writeLE16(os, 4)                  // block align
                writeLE16(os, 16)                 // 位深
                writeFourCC(os, "data")
                writeLE32(os, frames * 4)
                for (v in interleaved) {
                    val s = (Math.max(-1f, Math.min(1f, v)) * 32767f).toInt()
                    writeLE16(os, s and 0xFFFF)
                }
            }
        }

        private fun writeFourCC(o: DataOutputStream, s: String) {
            o.write(s.toByteArray(StandardCharsets.US_ASCII), 0, 4)
        }

        private fun writeLE32(o: DataOutputStream, v: Int) {
            o.write(v and 0xFF)
            o.write((v ushr 8) and 0xFF)
            o.write((v ushr 16) and 0xFF)
            o.write((v ushr 24) and 0xFF)
        }

        private fun writeLE16(o: DataOutputStream, v: Int) {
            o.write(v and 0xFF)
            o.write((v ushr 8) and 0xFF)
        }

        /** MIDI 变长数量（VLQ），高位在前。 */
        private fun vlq(out: ByteArrayOutputStream, value: Int) {
            var v = Math.max(0, value)
            val tmp = ArrayList<Int>()
            tmp.add(v and 0x7F)
            v = v ushr 7
            while (v > 0) {
                tmp.add((v and 0x7F) or 0x80)
                v = v ushr 7
            }
            for (i in tmp.indices.reversed()) out.write(tmp[i])
        }
    }

    // ---- 传输状态 ----
    @Volatile
    var playing = false
    var samplePos = 0L
    private var nextStepAbs = 0L

    @Volatile
    var stepIndex = 0
        private set
    private var globalStep = 0L
    private var cycleCount = 0
    private var fillNow = false
    private var pendingPattern = -1
    private var songIndex = 0
    private var songRepeat = 0
    private var rng: Int

    @Volatile
    var clockExternal = false
    private var clockPulse = 0

    /** 等于 0..15 时强制只听该轨（分轨导出用）。 */
    @Volatile
    var soloOverride = -1

    // ---- 给 UI 的只读指标 ----
    @Volatile
    var meterL = 0f

    @Volatile
    var meterR = 0f

    @Volatile
    var xruns = 0

    @Volatile
    var cpu = 0f

    private val voices = Array(16) { i -> Array(4) { XuDsp.Voice(i) } }

    // 手动触发（键盘/MIDI/点击）队列
    private val mT = LongArray(MAX_EVENTS)
    private val mPad = IntArray(MAX_EVENTS)
    private val mVel = FloatArray(MAX_EVENTS)
    private val mAcc = BooleanArray(MAX_EVENTS)
    private var mCount = 0

    // 音序器事件队列（一步可能拆出多个 ratchet）
    private val pT = LongArray(MAX_EVENTS)
    private val pPad = IntArray(MAX_EVENTS)
    private val pVel = FloatArray(MAX_EVENTS)
    private val pAcc = BooleanArray(MAX_EVENTS)
    private var pCount = 0

    // 本块要处理的事件
    private val bT = LongArray(MAX_EVENTS)
    private val bPad = IntArray(MAX_EVENTS)
    private val bVel = FloatArray(MAX_EVENTS)
    private val bAcc = BooleanArray(MAX_EVENTS)
    private var bCount = 0

    private val lpL = XuDsp.Biquad()
    private val lpR = XuDsp.Biquad()
    private val hpL = XuDsp.Biquad()
    private val hpR = XuDsp.Biquad()

    init {
        rng = if (proj.seed != 0) proj.seed else 0x808808
    }

    // ------------------------------------------------------------------
    // 传输
    // ------------------------------------------------------------------
    fun play() {
        if (playing) return
        playing = true
        stepIndex = 0
        globalStep = 0
        cycleCount = 0
        fillNow = false
        songIndex = 0
        songRepeat = 0
        nextStepAbs = samplePos
        pCount = 0
    }

    fun stop() {
        playing = false
        // 停止不是硬切：给 480 样本（10ms）淡出，避免“啪”声
        for (pool in voices) for (v in pool) v.kill(samplePos, 480)
        pCount = 0
    }

    fun toggle() {
        if (playing) stop() else play()
    }

    // ------------------------------------------------------------------
    // 时钟
    // ------------------------------------------------------------------
    /** 16 分音符步长（样本）。BPM 改变只影响下一步，不重置位置。 */
    private fun stepLen(): Int {
        val bpm = Math.max(40.0, Math.min(300.0, proj.master.bpm))
        return Math.max(64, Math.round(XuProject.SR * 60.0 / bpm / 4.0).toInt())
    }

    /** xorshift32：确定性随机，工程种子相同则每个版本的概率判定完全一致。 */
    private fun nextRand(): Int {
        var x = rng
        x = x xor (x shl 13)
        x = x xor (x ushr 17)
        x = x xor (x shl 5)
        rng = x
        return x
    }

    private fun condOk(cond: Int): Boolean = when (cond) {
        XuProject.COND_ALWAYS -> true
        XuProject.COND_FILL -> fillNow
        XuProject.COND_NOTFILL -> !fillNow
        XuProject.COND_EVERY2 -> (cycleCount % 2) == 0
        XuProject.COND_EVERY4 -> (cycleCount % 4) == 0
        else -> true
    }

    /** 入队；满了整体左移（丢最旧），保证内存有界且不扩容。 */
    private fun push(t: Long, pad: Int, vel: Float, accent: Boolean) {
        if (pCount >= MAX_EVENTS) {
            System.arraycopy(pT, 1, pT, 0, MAX_EVENTS - 1)
            System.arraycopy(pPad, 1, pPad, 0, MAX_EVENTS - 1)
            System.arraycopy(pVel, 1, pVel, 0, MAX_EVENTS - 1)
            System.arraycopy(pAcc, 1, pAcc, 0, MAX_EVENTS - 1)
            pCount--
        }
        pT[pCount] = t
        pPad[pCount] = pad
        pVel[pCount] = vel
        pAcc[pCount] = accent
        pCount++
    }

    /**
     * 发出当前步的全部事件。
     * 触发时刻 = 步起点 + swing 偏移 + 微时；ratchet 把一步均分。
     * 注意这里只算样本位置，不合成音频——合成统一在 render 里做。
     */
    private fun emitStep(stepAbs: Long) {
        val pat = proj.cur()
        val sl = stepLen()
        val swingOff = if ((stepIndex and 1) == 1) (sl * proj.master.swing * 0.5).toInt() else 0
        val base = stepAbs + swingOff
        for (pad in 0 until 16) {
            val tl = Math.max(1, Math.min(64, pat.trackLen[pad]))
            val idx = (globalStep % tl).toInt()
            val st = pat.data[pad][idx]
            if (!st.on || !condOk(st.cond) || st.prob <= 0) continue
            if (st.prob < 100 && (nextRand() % 100) >= st.prob) continue
            var vel = Math.max(0.02, Math.min(1.0, st.velocity / 127.0))
            if (st.accent) vel = Math.min(1.25, vel * 1.30)
            val r = Math.max(1, Math.min(8, st.ratchet))
            for (i in 0 until r) {
                val t = base + st.micro + (i * sl.toDouble() / r).toLong()
                push(t, pad, (vel * Math.pow(0.92, i.toDouble())).toFloat(), st.accent)
            }
        }
    }

    /** 一步结束后推进；Pattern 切换与 Song 链都在“步边界”发生。 */
    private fun advanceAfterStep() {
        val pat = proj.cur()
        stepIndex++
        globalStep++
        if (stepIndex < Math.max(1, pat.steps)) return
        stepIndex = 0
        cycleCount++
        fillNow = (cycleCount % 4) == 3           // 每 4 循环的最后一遍算“过门”
        if (pendingPattern >= 0) {
            proj.current = Math.max(0, Math.min(15, pendingPattern))
            pendingPattern = -1
            songRepeat = 0
            return
        }
        if (proj.master.songMode && proj.song.isNotEmpty()) {
            val en = proj.song[songIndex % proj.song.size]
            songRepeat++
            if (songRepeat >= Math.max(1, en.repeats)) {
                songRepeat = 0
                songIndex = (songIndex + 1) % proj.song.size
                proj.current = Math.max(0, Math.min(15, proj.song[songIndex].pattern))
            }
        }
    }

    /** Mute/Solo 在混音阶段判断，事件照样入队，取消静音后立刻恢复。 */
    private fun audible(pad: Int): Boolean {
        if (soloOverride >= 0) return pad == soloOverride
        var anySolo = false
        for (p in proj.pads) if (p.solo) {
            anySolo = true; break
        }
        val p = proj.pads[pad]
        if (p.mute) return false
        if (anySolo && !p.solo) return false
        return true
    }

    /** 触发声部：CH/OH 互相掐断；每声部 4 voice，超出抢最老的。 */
    private fun trigger(pad: Int, whenT: Long, vel: Float, accent: Boolean) {
        if (pad == 15 || pad == 14) {
            for (v in voices[14]) v.kill(whenT, 16)   // choke 组：闭镲掐开镲
        }
        var target = voices[pad].firstOrNull { !it.active }
        if (target == null) {
            val oldest = voices[pad].minByOrNull { it.start }!!
            oldest.kill(whenT, 64)
            target = oldest
        }
        target.trigger(whenT, vel.toDouble(), accent)
    }

    // ------------------------------------------------------------------
    // 渲染：由音频线程（或离线导出）调用
    // ------------------------------------------------------------------
    fun render(n: Int, L: FloatArray, R: FloatArray) {
        val b0 = samplePos
        val b1 = b0 + n
        Arrays.fill(L, 0, n, 0f)
        Arrays.fill(R, 0, n, 0f)
        collect(b0, b1)

        // 按事件时刻把块切成若干小段，逐段渲染当前所有 voice
        var cur = b0
        for (i in 0 until bCount) {
            val t = bT[i]
            if (t > cur) {
                renderVoices(L, R, n, b0, cur, t)
                cur = t
            }
            if (t < b1) trigger(bPad[i], t, bVel[i], bAcc[i])
        }
        if (cur < b1) renderVoices(L, R, n, b0, cur, b1)

        val m = proj.master
        // 效果链顺序固定：高通 → 低通 → 过载 → 软限幅 → 主音量
        if (m.hpOn) {
            hpL.setHighPass(m.hpCutoff, m.hpQ)
            hpR.setHighPass(m.hpCutoff, m.hpQ)
            for (i in 0 until n) {
                L[i] = hpL.run(L[i].toDouble()).toFloat()
                R[i] = hpR.run(R[i].toDouble()).toFloat()
            }
        }
        if (m.lpOn) {
            lpL.setLowPass(m.lpCutoff, m.lpQ)
            lpR.setLowPass(m.lpCutoff, m.lpQ)
            for (i in 0 until n) {
                L[i] = lpL.run(L[i].toDouble()).toFloat()
                R[i] = lpR.run(R[i].toDouble()).toFloat()
            }
        }
        val drive = Math.max(1.0, Math.min(8.0, m.drive))
        val norm = Math.tanh(drive)
        val vol = Math.max(0.0, Math.min(1.0, m.volume))
        var pl = 0f
        var pr = 0f
        for (i in 0 until n) {
            var xl = L[i].toDouble()
            var xr = R[i].toDouble()
            if (drive > 1.001) {
                xl = Math.tanh(xl * drive) / norm
                xr = Math.tanh(xr * drive) / norm
            }
            xl /= (1.0 + Math.abs(xl))
            xr /= (1.0 + Math.abs(xr))
            xl *= vol
            xr *= vol
            L[i] = xl.toFloat()
            R[i] = xr.toFloat()
            pl = Math.max(pl, Math.abs(xl).toFloat())
            pr = Math.max(pr, Math.abs(xr).toFloat())
        }
        // 表头做峰值保持 + 衰减，UI 只读这个值
        meterL = Math.max(pl, meterL * 0.75f)
        meterR = Math.max(pr, meterR * 0.75f)
        samplePos = b1
        for (pool in voices) for (v in pool) {
            if (v.active && b0 - v.start > 4L * XuProject.SR) v.active = false
        }
    }

    /** 收集本块的事件：手动队列 + 步进推进。 */
    private fun collect(b0: Long, b1: Long) {
        bCount = 0
        var keep = 0
        for (i in 0 until mCount) {
            val t = mT[i]
            if (t < b1) {
                addBlock(Math.max(t, b0), mPad[i], mVel[i], mAcc[i])
            } else {
                mT[keep] = t
                mPad[keep] = mPad[i]
                mVel[keep] = mVel[i]
                mAcc[keep] = mAcc[i]
                keep++
            }
        }
        mCount = keep

        if (playing && !clockExternal) {
            var guard = 0
            // 正常一块最多一两步；guard 防止 BPM 极端时死循环
            while (nextStepAbs < b1 && guard++ < 64) {
                emitStep(Math.max(nextStepAbs, b0))
                nextStepAbs += stepLen()
                advanceAfterStep()
            }
        }

        keep = 0
        for (i in 0 until pCount) {
            val t = pT[i]
            if (t < b1) {
                addBlock(Math.max(t, b0), pPad[i], pVel[i], pAcc[i])
            } else {
                pT[keep] = t
                pPad[keep] = pPad[i]
                pVel[keep] = pVel[i]
                pAcc[keep] = pAcc[i]
                keep++
            }
        }
        pCount = keep

        // 插入排序：每块事件只有几十个，比通用排序更快（且能同步四条数组）
        for (i in 1 until bCount) {
            val t = bT[i]
            val pd = bPad[i]
            val vv = bVel[i]
            val ac = bAcc[i]
            var j = i - 1
            while (j >= 0 && bT[j] > t) {
                bT[j + 1] = bT[j]
                bPad[j + 1] = bPad[j]
                bVel[j + 1] = bVel[j]
                bAcc[j + 1] = bAcc[j]
                j--
            }
            bT[j + 1] = t
            bPad[j + 1] = pd
            bVel[j + 1] = vv
            bAcc[j + 1] = ac
        }
    }

    private fun addBlock(t: Long, pad: Int, vel: Float, accent: Boolean) {
        if (bCount >= MAX_EVENTS) return
        bT[bCount] = t
        bPad[bCount] = pad
        bVel[bCount] = vel
        bAcc[bCount] = accent
        bCount++
    }

    private fun renderVoices(L: FloatArray, R: FloatArray, n: Int, b0: Long, a: Long, b: Long) {
        if (b <= a) return
        for (pad in 0 until 16) {
            if (!audible(pad)) continue
            val p = proj.pads[pad]
            for (v in voices[pad]) if (v.active) v.render(L, R, n, b0, b, p)
        }
    }

    // ------------------------------------------------------------------
    // 手动触发 / Pattern 请求 / 外部时钟
    // ------------------------------------------------------------------
    fun triggerPad(pad: Int, vel: Int, accent: Boolean) {
        if (pad < 0 || pad >= 16) return
        val v = Math.max(1, Math.min(127, vel))
        if (mCount >= MAX_EVENTS) return
        mT[mCount] = samplePos + 32      // 留一点提前量，下一块开头就发声
        mPad[mCount] = pad
        mVel[mCount] = v / 127.0f
        mAcc[mCount] = accent
        mCount++
    }

    /** 播放中切 Pattern 会挂起，等下一步边界再生效（不截断当前步）。 */
    fun requestPattern(idx: Int) {
        val i = Math.max(0, Math.min(15, idx))
        if (playing) pendingPattern = i
        else proj.current = i
    }

    /** 外部 MIDI 时钟：24 PPQN，6 个脉冲 = 一个 16 分音符。 */
    fun externalTick() {
        if (!playing) return
        if (++clockPulse < 6) return
        clockPulse = 0
        emitStep(samplePos)
        advanceAfterStep()
        nextStepAbs = samplePos + stepLen()
    }

    // ------------------------------------------------------------------
    // 离线渲染（与实时走同一份 render 代码，只把时钟换成固定块推进）
    // ------------------------------------------------------------------
    fun renderPattern(repeats: Int = 1): FloatArray {
        val copy = proj.copyProject()
        val e = XuEngine(copy)
        return e.renderSeconds(patternSeconds(copy, e, repeats))
    }

    fun renderSong(): FloatArray {
        val copy = proj.copyProject()
        val e = XuEngine(copy)
        return e.renderSeconds(songSeconds(copy, e))
    }

    private fun patternSeconds(copy: XuProject, e: XuEngine, repeats: Int): Double {
        val steps = copy.cur().steps * Math.max(1, repeats)
        return steps * e.stepLen().toDouble() / XuProject.SR + 3.0   // 尾巴 3s，等最后一击自然衰减
    }

    private fun songSeconds(copy: XuProject, e: XuEngine): Double {
        var steps = 0
        if (copy.song.isNotEmpty()) {
            for (en in copy.song) {
                val idx = Math.max(0, Math.min(15, en.pattern))
                steps += copy.patterns[idx].steps * Math.max(1, en.repeats)
            }
        }
        if (steps <= 0) steps = copy.cur().steps
        return steps * e.stepLen().toDouble() / XuProject.SR + 3.0
    }

    private fun renderSeconds(seconds: Double): FloatArray {
        val total = (seconds * XuProject.SR).toInt()
        val out = FloatArray(total * 2)
        val L = FloatArray(BLOCK)
        val R = FloatArray(BLOCK)
        play()
        var done = 0
        while (done < total) {
            val n = Math.min(BLOCK, total - done)
            render(n, L, R)
            for (i in 0 until n) {
                out[(done + i) * 2] = L[i]
                out[(done + i) * 2 + 1] = R[i]
            }
            done += n
        }
        return out
    }

    fun exportWav(path: String, song: Boolean) {
        writeWav(path, if (song) renderSong() else renderPattern())
    }

    /** 分轨导出：逐轨 solo 渲染，得到 16 个单声道内容的立体声 WAV。 */
    fun exportStems(basePath: String, song: Boolean) {
        val base = basePath.removeSuffix(".wav").removeSuffix(".WAV")
        for (pad in 0 until 16) {
            val copy = proj.copyProject()
            val e = XuEngine(copy)
            e.soloOverride = pad
            val sec = if (song) songSeconds(copy, e) else patternSeconds(copy, e, 1)
            writeWav("${base}_${XuProject.CODES[pad]}.wav", e.renderSeconds(sec))
        }
    }

    // ------------------------------------------------------------------
    // MIDI 导出：SMF Format 1，PPQ=480，16 分音符 = 120 tick
    // ------------------------------------------------------------------
    fun exportMidi(path: String, song: Boolean) {
        val ppq = 480
        val bpm = Math.max(40.0, Math.min(300.0, proj.master.bpm))
        val stepSamples = XuProject.SR * 60.0 / bpm / 4.0

        // 1) 先把要导出的 Pattern 序列摊平（单 Pattern 或整条 Song）
        val seq = ArrayList<Int>()
        if (song && proj.song.isNotEmpty()) {
            for (en in proj.song)
                for (k in 0 until Math.max(1, en.repeats))
                    seq.add(Math.max(0, Math.min(15, en.pattern)))
        } else {
            seq.add(proj.current)
        }

        // 2) 逐个格子展开音符（含 swing / 微时 / ratchet / 重音）
        val notes = ArrayList<IntArray>()
        var cursor = 0
        for (pi in seq) {
            val pat = proj.patterns[pi]
            for (s in 0 until pat.steps) {
                val swingTick = if (s % 2 == 1) (proj.master.swing * 60.0).toInt() else 0
                for (pad in 0 until 16) {
                    val tl = Math.max(1, Math.min(64, pat.trackLen[pad]))
                    val idx = ((cursor + s) % tl)
                    val st = pat.data[pad][idx]
                    if (!st.on || st.prob <= 0) continue
                    val vel = Math.max(1, Math.min(127, (st.velocity * (if (st.accent) 1.3 else 1.0)).toInt()))
                    val baseTick = (cursor + s) * 120
                    val microTick = Math.round(st.micro / stepSamples * 120.0).toInt()
                    val r = Math.max(1, Math.min(8, st.ratchet))
                    for (i in 0 until r) {
                        val tick = Math.max(0, baseTick + swingTick + microTick + (i * 120.0 / r).toInt())
                        notes.add(intArrayOf(tick, XuProject.MIDI_NOTES[pad], vel))
                    }
                }
            }
            cursor += pat.steps
        }
        notes.sortWith(Comparator { a, b ->
            if (a[0] != b[0]) a[0].compareTo(b[0]) else a[1].compareTo(b[1])
        })

        // 3) 组装单轨 SMF
        val track = ByteArrayOutputStream()
        val name = "XU-808 by XuMou".toByteArray(StandardCharsets.US_ASCII)
        vlq(track, 0); track.write(0xFF); track.write(0x03); vlq(track, name.size)
        track.write(name, 0, name.size)

        val tempo = (60000000.0 / bpm).toInt()
        vlq(track, 0); track.write(0xFF); track.write(0x51); track.write(0x03)
        track.write((tempo shr 16) and 0xFF)
        track.write((tempo shr 8) and 0xFF)
        track.write(tempo and 0xFF)

        vlq(track, 0); track.write(0xFF); track.write(0x58); track.write(0x04)
        track.write(proj.master.beatsPerBar)
        var bu = Math.max(1, proj.master.beatUnit)
        var du = 0
        while (bu > 1) {
            bu = bu shr 1; du++
        }
        track.write(du)      // 拍号分母是 2 的幂次
        track.write(24)      // 每 MIDI 时钟的 tick 数
        track.write(8)       // 32 分音符数/四分音符

        var last = 0
        for (nt in notes) {
            vlq(track, nt[0] - last)
            track.write(0x99)                 // Note On, channel 10（0 基）
            track.write(nt[1])
            track.write(nt[2])
            vlq(track, 10)
            track.write(0x89)                 // Note Off
            track.write(nt[1])
            track.write(0x40)
            last = nt[0] + 10
        }
        vlq(track, 0); track.write(0xFF); track.write(0x2F); track.write(0x00)

        DataOutputStream(BufferedOutputStream(FileOutputStream(path))).use { os ->
            writeFourCC(os, "MThd"); writeLE32(os, 6); writeLE16(os, 1); writeLE16(os, 1); writeLE16(os, ppq)
            writeFourCC(os, "MTrk"); writeLE32(os, track.size()); os.write(track.toByteArray())
        }
    }
}
