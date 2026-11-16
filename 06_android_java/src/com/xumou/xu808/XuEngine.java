// ============================================================================
//  XuEngine.java —— 采样级音序器 + 混音 + 离线导出（Android 版）
//
//  徐某的设计要点：
//    · 主时钟只在音频线程里累计 samplePos，绝不用 UI 定时器当节拍源；
//    · 事件队列用四条平行定长数组（时刻/声部/力度/重音），容量固定，
//      满了丢最旧，回调里永不扩容，避免 GC 抖动；
//    · 所有步进/摇摆/微时/ratchet 都在“样本”域计算，整数运算不漂移；
//    · 导出复用同一套 render()，只把时钟换成固定块推进。
// ============================================================================
package com.xumou.xu808;

import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class XuEngine {
    public static final int BLOCK = 512;              // 512 样本 @48k ≈ 10.7ms（规格书）
    private static final int MAX_EVENTS = 4096;
    private final XuDsp.Voice[][] voices = new XuDsp.Voice[16][4];  // 每声部 4 个并发 voice
    // 待处理事件（跨块保留）
    private final long[] pT = new long[MAX_EVENTS];
    private final int[] pPad = new int[MAX_EVENTS];
    private final float[] pVel = new float[MAX_EVENTS];
    private final boolean[] pAcc = new boolean[MAX_EVENTS];
    // 本块要处理的事件（已按时间排序）
    private final long[] bT = new long[MAX_EVENTS];
    private final int[] bPad = new int[MAX_EVENTS];
    private final float[] bVel = new float[MAX_EVENTS];
    private final boolean[] bAcc = new boolean[MAX_EVENTS];
    // 主滤波：左右各一套，保证立体声状态独立
    private final XuDsp.Biquad lpL = new XuDsp.Biquad(), lpR = new XuDsp.Biquad();
    private final XuDsp.Biquad hpL = new XuDsp.Biquad(), hpR = new XuDsp.Biquad();
    // ------------------------------------------------------------------
    // 手动试听 / Pattern 请求 / 外部时钟
    // ------------------------------------------------------------------
    private final long[] mT = new long[MAX_EVENTS];
    private final int[] mPad = new int[MAX_EVENTS];
    private final float[] mVel = new float[MAX_EVENTS];
    private final boolean[] mAcc = new boolean[MAX_EVENTS];
    public XuProject proj;
    public volatile boolean playing = false;
    public volatile long samplePos = 0;
    public boolean clockExternal = false;             // 外部 MIDI 时钟
    public int soloOverride = -1;                     // 分轨导出用：-1 表示不覆盖
    // 供 UI 读取的环形快照（volatile 单写单读足够）
    public volatile float meterL = 0, meterR = 0;
    public volatile int xruns = 0;
    public volatile float cpu = 0;
    private long nextStepAbs = 0;
    private volatile int stepIndex = 0;               // UI 只读：当前织体步号
    private long globalStep = 0;                      // 从播放开始累计的全局步号
    private int cycleCount = 0;                       // Pattern 循环次数（条件触发用）
    private boolean fillNow = false;
    private volatile int pendingPattern = -1;         // 下一步边界才切换（UI 线程写，音频线程读）
    private int songIndex = 0, songRepeat = 0;
    private int rng;                                  // xorshift32 状态
    private int clockPulse = 0;
    private int pCount = 0;
    private int bCount = 0;
    private int mCount = 0;

    public XuEngine(XuProject p) {
        proj = p;
        rng = p.seed != 0 ? p.seed : 0x808808;
        for (int i = 0; i < 16; i++)
            for (int v = 0; v < 4; v++) voices[i][v] = new XuDsp.Voice(i);
    }

    // ------------------------------------------------------------------
    // 时钟与随机
    // ------------------------------------------------------------------

    /**
     * 写 16bit PCM WAV；流式写，长歌曲也不会吃光内存。
     */
    public static void writeWav(File f, float[] interleaved) throws IOException {
        int frames = interleaved.length / 2;
        DataOutputStream os = null;
        try {
            os = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(f), 1 << 16));
            writeFourCC(os, "RIFF");
            writeLE32(os, 36 + frames * 4);
            writeFourCC(os, "WAVE");
            writeFourCC(os, "fmt ");
            writeLE32(os, 16);
            writeLE16(os, 1);            // PCM
            writeLE16(os, 2);            // 立体声
            writeLE32(os, XuProject.SR);
            writeLE32(os, XuProject.SR * 4);
            writeLE16(os, 4);
            writeLE16(os, 16);
            writeFourCC(os, "data");
            writeLE32(os, frames * 4);
            for (float v : interleaved) {
                int s = (int) (Math.max(-1f, Math.min(1f, v)) * 32767f);
                writeLE16(os, s & 0xFFFF);
            }
        } finally {
            if (os != null) try {
                os.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void vlq(ByteArrayOutputStream out, int v) {
        v = Math.max(0, v);
        ArrayList<Integer> tmp = new ArrayList<>();
        tmp.add(v & 0x7F);
        v >>= 7;
        while (v > 0) {
            tmp.add((v & 0x7F) | 0x80);
            v >>= 7;
        }
        for (int i = tmp.size() - 1; i >= 0; i--) out.write(tmp.get(i));
    }

    private static void writeFourCC(DataOutputStream o, String s) throws IOException {
        o.write(s.getBytes(StandardCharsets.US_ASCII), 0, 4);
    }

    private static void writeLE32(DataOutputStream o, int v) throws IOException {
        o.write(v & 0xFF);
        o.write((v >> 8) & 0xFF);
        o.write((v >> 16) & 0xFF);
        o.write((v >> 24) & 0xFF);
    }

    private static void writeLE16(DataOutputStream o, int v) throws IOException {
        o.write(v & 0xFF);
        o.write((v >> 8) & 0xFF);
    }

    // ------------------------------------------------------------------
    // 传输控制
    // ------------------------------------------------------------------
    public void play() {
        if (playing) return;
        playing = true;
        stepIndex = 0;
        globalStep = 0;
        cycleCount = 0;
        fillNow = false;
        songIndex = 0;
        songRepeat = 0;
        nextStepAbs = samplePos;      // 从当前样本位置起步，避免“迟到”
        pCount = 0;
    }

    public void stop() {
        playing = false;
        // 停止时给 480 样本淡出，杜绝“硬切”爆音和残留
        for (XuDsp.Voice[] pool : voices)
            for (XuDsp.Voice v : pool) v.kill(samplePos, 480);
        pCount = 0;
        stepIndex = 0;
    }

    public void toggle() {
        if (playing) stop();
        else play();
    }

    public int getStepIndex() {
        return stepIndex;
    }

    public long getGlobalStep() {
        return globalStep;
    }

    /**
     * 一步 = 16 分音符 = 48000*60/BPM/4 样本；BPM 实时变化只影响下一步。
     */
    private int stepLen() {
        float bpm = Math.max(40f, Math.min(300f, proj.master.bpm));
        return Math.max(64, (int) Math.round(XuProject.SR * 60.0 / bpm / 4.0));
    }

    /**
     * xorshift32：确定性随机，同一工程每次播放的概率结果一致。
     */
    private int nextRand() {
        int x = rng;
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        rng = x;
        return x;
    }

    private boolean condOk(int cond) {
        switch (cond) {
            case XuProject.COND_ALWAYS:
                return true;
            case XuProject.COND_FILL:
                return fillNow;
            case XuProject.COND_NOTFILL:
                return !fillNow;
            case XuProject.COND_EVERY2:
                return (cycleCount % 2) == 0;
            case XuProject.COND_EVERY4:
                return (cycleCount % 4) == 0;
            default:
                return true;
        }
    }

    private void push(long t, int pad, float vel, boolean accent) {
        if (pCount >= MAX_EVENTS) {
            // 队列满：整体左移，丢弃最旧事件，保持内存有界
            System.arraycopy(pT, 1, pT, 0, MAX_EVENTS - 1);
            System.arraycopy(pPad, 1, pPad, 0, MAX_EVENTS - 1);
            System.arraycopy(pVel, 1, pVel, 0, MAX_EVENTS - 1);
            System.arraycopy(pAcc, 1, pAcc, 0, MAX_EVENTS - 1);
            pCount--;
        }
        pT[pCount] = t;
        pPad[pCount] = pad;
        pVel[pCount] = vel;
        pAcc[pCount] = accent;
        pCount++;
    }

    /**
     * 展开一步：16 轨各自按 trackLen 取格子，处理条件/概率，
     * 再按 swing+micro 计算触发样本，并把 ratchet 拆成 N 个子事件。
     */
    private void emitStep(long stepAbs) {
        XuProject.Pattern pat = proj.cur();
        int sl = stepLen();
        // 奇数 16 分步向后偏移 swing * stepSamples * 0.5
        int swingOff = (stepIndex & 1) == 1 ? (int) (sl * proj.master.swing * 0.5) : 0;
        long base = stepAbs + swingOff;
        for (int pad = 0; pad < 16; pad++) {
            int tl = Math.max(1, Math.min(64, pat.trackLen[pad]));
            int idx = (int) (globalStep % tl);        // polymeter：轨道自己的步号
            XuProject.Step st = pat.data[pad][idx];
            if (!st.on || !condOk(st.cond) || st.prob <= 0) continue;
            // 概率只在触发时刻判定一次
            if (st.prob < 100 && (nextRand() % 100) >= st.prob) continue;
            float vel = Math.max(0.02f, Math.min(1f, st.velocity / 127.0f));
            // 重音：音量 +30%，合成层还会额外改变包络/音色
            if (st.accent) vel = Math.min(1.25f, vel * 1.30f);
            int r = Math.max(1, Math.min(8, st.ratchet));
            for (int i = 0; i < r; i++) {
                // ratchet 子事件均分一步，力度逐级递减 8%
                long t = base + st.micro + (long) (i * (double) sl / r);
                push(t, pad, (float) (vel * Math.pow(0.92, i)), st.accent);
            }
        }
    }

    /**
     * 步进推进：到 Pattern 末尾回零，并在这里处理 Pattern/歌曲链切换。
     */
    private void advanceAfterStep() {
        XuProject.Pattern pat = proj.cur();
        stepIndex++;
        globalStep++;
        if (stepIndex < Math.max(1, pat.steps)) return;
        stepIndex = 0;
        cycleCount++;
        // 每 4 个循环自动进入一次过门；也可以手动把 Pattern 标成 fill
        fillNow = (cycleCount % 4) == 3 || pat.fill;
        if (pendingPattern >= 0) {
            proj.current = Math.max(0, Math.min(15, pendingPattern));
            pendingPattern = -1;
            songRepeat = 0;
            return;
        }
        if (proj.master.songMode && !proj.song.isEmpty()) {
            XuProject.SongEntry en = proj.song.get(songIndex % proj.song.size());
            songRepeat++;
            if (songRepeat >= Math.max(1, en.repeats)) {
                songRepeat = 0;
                songIndex = (songIndex + 1) % proj.song.size();
                proj.current = Math.max(0, Math.min(15, proj.song.get(songIndex).pattern));
            }
        }
    }

    /**
     * Mute/Solo 判定：在混音阶段以原子标志阻断，不进合成器。
     */
    private boolean audible(int pad) {
        if (soloOverride >= 0) return pad == soloOverride;
        boolean anySolo = false;
        for (XuProject.Pad p : proj.pads)
            if (p.solo) {
                anySolo = true;
                break;
            }
        XuProject.Pad p = proj.pads[pad];
        if (p.mute) return false;
        if (anySolo && !p.solo) return false;
        return true;
    }

    /**
     * 触发一个 voice：CHOKE 组 + 空闲优先 + 最老抢占。
     */
    private void trigger(int pad, long when, float vel, boolean accent) {
        // 闭镲/开镲互相掐断（CHOKE 组 1）
        if (pad == 15 || pad == 14)
            for (XuDsp.Voice v : voices[14]) v.kill(when, 16);
        XuDsp.Voice target = null;
        for (XuDsp.Voice v : voices[pad])
            if (!v.active) {
                target = v;
                break;
            }
        if (target == null) {
            // 四个 voice 都在响：抢占开始最早的，并做 64 样本淡出
            target = voices[pad][0];
            for (XuDsp.Voice v : voices[pad]) if (v.start < target.start) target = v;
            target.kill(when, 64);
        }
        target.trigger(when, vel, accent);
    }

    // ------------------------------------------------------------------
    // 实时渲染：一个块 = BLOCK 个样本
    // ------------------------------------------------------------------
    public void render(int n, float[] L, float[] R) {
        long b0 = samplePos, b1 = b0 + n;
        Arrays.fill(L, 0, n, 0f);
        Arrays.fill(R, 0, n, 0f);
        collect(b0, b1);                       // 收集本块事件并推进音序

        long cur = b0;
        for (int i = 0; i < bCount; i++) {
            long t = bT[i];
            if (t > cur) {
                renderVoices(L, R, n, b0, cur, t);
                cur = t;
            }
            if (t < b1) trigger(bPad[i], t, bVel[i], bAcc[i]);
        }
        if (cur < b1) renderVoices(L, R, n, b0, cur, b1);

        // ---- 主效果链：HP → LP → tanh 过载 → 软限幅 → 主音量 ----
        XuProject.Master m = proj.master;
        if (m.hpOn) {
            hpL.setHighPass(m.hpCutoff, m.hpQ);
            hpR.setHighPass(m.hpCutoff, m.hpQ);
            for (int i = 0; i < n; i++) {
                L[i] = (float) hpL.run(L[i]);
                R[i] = (float) hpR.run(R[i]);
            }
        }
        if (m.lpOn) {
            lpL.setLowPass(m.lpCutoff, m.lpQ);
            lpR.setLowPass(m.lpCutoff, m.lpQ);
            for (int i = 0; i < n; i++) {
                L[i] = (float) lpL.run(L[i]);
                R[i] = (float) lpR.run(R[i]);
            }
        }
        float drive = Math.max(1f, Math.min(8f, m.drive));
        float norm = (float) Math.tanh(drive);
        float vol = Math.max(0f, Math.min(1f, m.volume));
        float pl = 0, pr = 0;
        for (int i = 0; i < n; i++) {
            float xl = L[i], xr = R[i];
            if (drive > 1.001f) {
                // 归一化 tanh 过载：drive=1 时近似直通
                xl = (float) Math.tanh(xl * drive) / norm;
                xr = (float) Math.tanh(xr * drive) / norm;
            }
            // 软限幅，保证任何情况下都不削波
            xl = xl / (1f + Math.abs(xl));
            xr = xr / (1f + Math.abs(xr));
            xl *= vol;
            xr *= vol;
            L[i] = xl;
            R[i] = xr;
            pl = Math.max(pl, Math.abs(xl));
            pr = Math.max(pr, Math.abs(xr));
        }
        // 电平表带缓慢回落，视觉上更稳
        meterL = Math.max(pl, meterL * 0.75f);
        meterR = Math.max(pr, meterR * 0.75f);
        samplePos = b1;

        // 兜底回收：任何超过 4 秒仍在“活跃”的 voice 都强制释放
        for (XuDsp.Voice[] pool : voices)
            for (XuDsp.Voice v : pool)
                if (v.active && b0 - v.start > 4L * XuProject.SR) v.active = false;
    }

    /**
     * 把跨块事件搬进本块，并让音序器把落点在本块内的步展开成事件。
     */
    private void collect(long b0, long b1) {
        bCount = 0;
        int keep = 0;
        // 先处理手动试听事件（triggerPad 推入的）
        for (int i = 0; i < mCount; i++) {
            long t = mT[i];
            if (t < b1) {
                addBlock(Math.max(t, b0), mPad[i], mVel[i], mAcc[i]);
            } else {
                mT[keep] = t;
                mPad[keep] = mPad[i];
                mVel[keep] = mVel[i];
                mAcc[keep] = mAcc[i];
                keep++;
            }
        }
        mCount = keep;

        // 内部时钟：严格按样本推进，ui 定时器完全不参与
        if (playing && !clockExternal) {
            int guard = 0;
            while (nextStepAbs < b1 && guard++ < 64) {
                emitStep(Math.max(nextStepAbs, b0));
                nextStepAbs += stepLen();
                advanceAfterStep();
            }
        }

        // 再处理音序器事件（可能跨越多个块）
        keep = 0;
        for (int i = 0; i < pCount; i++) {
            long t = pT[i];
            if (t < b1) {
                addBlock(Math.max(t, b0), pPad[i], pVel[i], pAcc[i]);
            } else {
                pT[keep] = t;
                pPad[keep] = pPad[i];
                pVel[keep] = pVel[i];
                pAcc[keep] = pAcc[i];
                keep++;
            }
        }
        pCount = keep;

        // 插入排序（事件很少，比 Arrays.sort 省，且能同步四条平行数组）
        for (int i = 1; i < bCount; i++) {
            long t = bT[i];
            int pd = bPad[i];
            float vv = bVel[i];
            boolean ac = bAcc[i];
            int j = i - 1;
            while (j >= 0 && bT[j] > t) {
                bT[j + 1] = bT[j];
                bPad[j + 1] = bPad[j];
                bVel[j + 1] = bVel[j];
                bAcc[j + 1] = bAcc[j];
                j--;
            }
            bT[j + 1] = t;
            bPad[j + 1] = pd;
            bVel[j + 1] = vv;
            bAcc[j + 1] = ac;
        }
    }

    private void addBlock(long t, int pad, float vel, boolean accent) {
        if (bCount >= MAX_EVENTS) return;
        bT[bCount] = t;
        bPad[bCount] = pad;
        bVel[bCount] = vel;
        bAcc[bCount] = accent;
        bCount++;
    }

    /**
     * 渲染 [a, b) 区间内所有可听声部的活跃 voice。
     */
    private void renderVoices(float[] L, float[] R, int n, long b0, long a, long b) {
        if (b <= a) return;
        for (int pad = 0; pad < 16; pad++) {
            if (!audible(pad)) continue;
            XuProject.Pad p = proj.pads[pad];
            for (XuDsp.Voice v : voices[pad])
                if (v.active) v.render(L, R, n, b0, b, p);
        }
    }

    /**
     * 点击网格或按键试听：延迟 32 样本（<1ms），避免与当前块边界竞争。
     */
    public void triggerPad(int pad, int vel, boolean accent) {
        if (pad < 0 || pad >= 16) return;
        vel = Math.max(1, Math.min(127, vel));
        if (mCount >= MAX_EVENTS) return;
        mT[mCount] = samplePos + 32;
        mPad[mCount] = pad;
        mVel[mCount] = vel / 127.0f;
        mAcc[mCount] = accent;
        mCount++;
    }

    /**
     * 切换 Pattern：播放中排队到下一步边界，停止时立即生效。
     */
    public void requestPattern(int idx) {
        idx = Math.max(0, Math.min(15, idx));
        if (playing) pendingPattern = idx;
        else proj.current = idx;
    }

    /**
     * 外部 MIDI 时钟：24 PPQN，6 个脉冲 = 1 个 16 分步。
     */
    public void externalTick() {
        if (!playing) return;
        if (++clockPulse < 6) return;
        clockPulse = 0;
        emitStep(samplePos);
        advanceAfterStep();
        nextStepAbs = samplePos + stepLen();
    }

    // ------------------------------------------------------------------
    // 离线渲染与导出（WAV / MIDI）
    // ------------------------------------------------------------------
    public float[] renderPattern() {
        return renderPattern(1);
    }

    public float[] renderPattern(int repeats) {
        XuProject copy = XuProject.fromMap(proj.toMap());
        XuEngine e = new XuEngine(copy);
        int steps = copy.cur().steps * Math.max(1, repeats);
        double sec = steps * (double) e.stepLen() / XuProject.SR + 3.0;
        return e.renderSeconds(sec);
    }

    public float[] renderSong() {
        XuProject copy = XuProject.fromMap(proj.toMap());
        XuEngine e = new XuEngine(copy);
        int steps = 0;
        if (!copy.song.isEmpty()) {
            for (XuProject.SongEntry en : copy.song) {
                int idx = Math.max(0, Math.min(15, en.pattern));
                steps += copy.patterns[idx].steps * Math.max(1, en.repeats);
            }
        }
        if (steps <= 0) steps = copy.cur().steps;
        double sec = steps * (double) e.stepLen() / XuProject.SR + 3.0;
        return e.renderSeconds(sec);
    }

    /**
     * 固定的块推进离线渲染（与实时完全同一套代码）。
     */
    private float[] renderSeconds(double seconds) {
        int total = (int) (seconds * XuProject.SR);
        float[] out = new float[total * 2];
        float[] L = new float[BLOCK], R = new float[BLOCK];
        play();
        int done = 0;
        while (done < total) {
            int n = Math.min(BLOCK, total - done);
            render(n, L, R);
            for (int i = 0; i < n; i++) {
                out[(done + i) * 2] = L[i];
                out[(done + i) * 2 + 1] = R[i];
            }
            done += n;
        }
        return out;
    }

    public void exportWav(File f, boolean song) throws IOException {
        writeWav(f, song ? renderSong() : renderPattern());
    }

    /**
     * 分轨导出：16 个 WAV，每轨单独 solo 渲染。
     * 徐某：分轨时长与整曲一致，方便直接拖进 DAW 对齐。
     */
    public void exportStems(File dir, boolean song) throws IOException {
        if (!dir.exists()) dir.mkdirs();
        double sec;
        if (song && !proj.song.isEmpty()) {
            int steps = 0;
            for (XuProject.SongEntry en : proj.song)
                steps += proj.patterns[Math.max(0, Math.min(15, en.pattern))].steps
                        * Math.max(1, en.repeats);
            sec = steps * (double) stepLen() / XuProject.SR + 3.0;
        } else {
            sec = proj.cur().steps * (double) stepLen() / XuProject.SR + 3.0;
        }
        for (int pad = 0; pad < 16; pad++) {
            XuProject copy = XuProject.fromMap(proj.toMap());
            XuEngine e = new XuEngine(copy);
            e.soloOverride = pad;
            float[] data = e.renderSeconds(sec);
            String nn = (pad < 10 ? "0" : "") + pad;
            writeWav(new File(dir, "stem_" + nn + "_" + XuProject.CODES[pad] + ".wav"), data);
        }
    }

    /**
     * 导出 SMF Format 1：PPQ=480，16 分音符 = 120 tick。
     */
    public void exportMidi(File f, boolean song) throws IOException {
        int ppq = 480;
        double bpm = Math.max(40, Math.min(300, proj.master.bpm));
        double stepSamples = XuProject.SR * 60.0 / bpm / 4.0;

        // 先展开要导出的 Pattern 序列（单 Pattern 或整首歌曲链）
        List<Integer> seq = new ArrayList<>();
        if (song && !proj.song.isEmpty()) {
            for (XuProject.SongEntry en : proj.song)
                for (int k = 0; k < Math.max(1, en.repeats); k++)
                    seq.add(Math.max(0, Math.min(15, en.pattern)));
        } else {
            seq.add(proj.current);
        }

        ArrayList<int[]> notes = new ArrayList<>();
        long cursor = 0;
        for (int pi : seq) {
            XuProject.Pattern pat = proj.patterns[pi];
            for (int s = 0; s < pat.steps; s++) {
                // swing 换算成 tick（swing 0.75 → 90 tick 偏移）
                int swingTick = (s % 2 == 1) ? (int) (proj.master.swing * 60.0) : 0;
                for (int pad = 0; pad < 16; pad++) {
                    int tl = Math.max(1, Math.min(64, pat.trackLen[pad]));
                    int idx = (int) ((cursor + s) % tl);
                    XuProject.Step st = pat.data[pad][idx];
                    if (!st.on || st.prob <= 0) continue;
                    int vel = Math.max(1, Math.min(127,
                            (int) (st.velocity * (st.accent ? 1.3f : 1f))));
                    int baseTick = (int) ((cursor + s) * 120);
                    // 微时（样本）→ tick，并钳在 ±24 tick（规格书第 7 节）
                    int microTick = (int) Math.round(st.micro / stepSamples * 120.0);
                    microTick = Math.max(-24, Math.min(24, microTick));
                    int r = Math.max(1, Math.min(8, st.ratchet));
                    for (int i = 0; i < r; i++) {
                        int tick = Math.max(0, baseTick + swingTick + microTick + (int) (i * 120.0 / r));
                        notes.add(new int[]{tick, XuProject.MIDI_NOTES[pad], vel});
                    }
                }
            }
            cursor += pat.steps;
        }
        // 徐某：不用 lambda，避免 javac 在 android.jar 引导类路径下找不到 LambdaMetafactory
        notes.sort(new java.util.Comparator<int[]>() {
            @Override
            public int compare(int[] a, int[] b) {
                if (a[0] != b[0]) return Integer.compare(a[0], b[0]);
                return Integer.compare(a[1], b[1]);
            }
        });

        // 组装 MTrk：轨名 → 速度 → 拍号 → 音符
        ByteArrayOutputStream track = new ByteArrayOutputStream();
        byte[] name = "XU-808 by XuMou".getBytes(StandardCharsets.US_ASCII);
        vlq(track, 0);
        track.write(0xFF);
        track.write(0x03);
        vlq(track, name.length);
        track.write(name, 0, name.length);
        int tempo = (int) (60000000.0 / bpm);
        vlq(track, 0);
        track.write(0xFF);
        track.write(0x51);
        track.write(0x03);
        track.write((tempo >> 16) & 0xFF);
        track.write((tempo >> 8) & 0xFF);
        track.write(tempo & 0xFF);
        vlq(track, 0);
        track.write(0xFF);
        track.write(0x58);
        track.write(0x04);
        track.write(proj.master.beatsPerBar);
        int du = 0, bu = Math.max(1, proj.master.beatUnit);
        while (bu > 1) {
            bu >>= 1;
            du++;
        }
        track.write(du);
        track.write(24);
        track.write(8);   // 24 MIDI clocks/quarter, 8 32nds

        int last = 0;
        for (int[] nt : notes) {
            vlq(track, nt[0] - last);
            track.write(0x99);
            track.write(nt[1]);
            track.write(nt[2]);
            vlq(track, 10);
            track.write(0x89);
            track.write(nt[1]);
            track.write(0x40);
            last = nt[0] + 10;
        }
        vlq(track, 0);
        track.write(0xFF);
        track.write(0x2F);
        track.write(0x00);

        DataOutputStream os = null;
        try {
            os = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(f)));
            writeFourCC(os, "MThd");
            writeLE32(os, 6);
            writeLE16(os, 1);
            writeLE16(os, 1);
            writeLE16(os, ppq);
            writeFourCC(os, "MTrk");
            writeLE32(os, track.size());
            os.write(track.toByteArray());
        } finally {
            if (os != null) try {
                os.close();
            } catch (IOException ignored) {
            }
        }
    }
}
