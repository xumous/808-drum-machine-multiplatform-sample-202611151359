/* ============================================================================
 * xu808-worklet.js —— AudioWorklet 内的实时 808 引擎（09/10 两版共用）
 * ----------------------------------------------------------------------------
 * 徐某为什么把整台鼓机塞进 worklet：
 *   1) 规格书明令禁止 ScriptProcessor，AudioWorklet 的 process() 运行在音频
 *      渲染线程，128 帧一个量子，天然与主线程事件循环解耦；
 *   2) 音序时钟必须由音频线程的 frameCount 累计，绝不能拿 setInterval 当拍子；
 *   3) 这里逐样本复刻 02_java_swing 的 XuDsp/XuEngine 公式，
 *      保证浏览器听到的音色与服务端导出的 WAV 完全同源。
 *
 * 与 Java 版的对应关系：
 *   Biquad      <-> XuDsp.Biquad（RBJ 双二阶）
 *   Voice       <-> XuDsp.Voice（16 声部合成模型 + 噪声池 + 包络）
 *   Engine      <-> XuEngine（运输、步进、swing/概率/微时/ratchet、混音）
 * 差异只有一处：Java 用 long 计样本，这里用 Number（2^53 内精确），
 * 连续播放 3000 年以上才会溢出，轮不到我们担心。
 * ============================================================================ */

/* AudioWorkletGlobalScope 自带 sampleRate，徐某直接拿来当唯一采样率常量 */
const SR = sampleRate;
const TWO_PI = Math.PI * 2;

/* ----------------------------------------------------------------------------
 * JavaRandom：逐位复刻 java.util.Random(seed)
 * 为什么要复刻：Java 版每个 Voice 用 new Random(0x808 + pad*7919) 生成噪声池，
 * 如果这里用 Math.random()，同一个工程在浏览器与 WAV 导出里噪声纹理就不一样。
 * 48 位乘法超出 IEEE754 的 53 位整数安全范围，所以初始化阶段用 BigInt；
 * 只有上电建表时跑 16×8192 次，这点开销完全不影响实时线程。
 * -------------------------------------------------------------------------- */
class JavaRandom {
    constructor(seed) {
        // Java 构造器第一件事就是 seed = (seed ^ multiplier) & mask
        this.seed = (BigInt(seed) ^ 0x5DEECE66Dn) & ((1n << 48n) - 1n);
    }

    /** 对应 Java Random.next(bits) */
    next(bits) {
        this.seed = (this.seed * 0x5DEECE66Dn + 0xBn) & ((1n << 48n) - 1n);
        // 取高 bits 位，转回 Number 时最大 2^32，绝对安全
        return Number(this.seed >> BigInt(48 - bits));
    }

    /** 对应 Java Random.nextDouble()：(next(26) << 27 + next(27)) / 2^53 */
    nextDouble() {
        return ((this.next(26) * 134217728) + this.next(27)) / 9007199254740992;
    }
}

/* 每个声部一张 8192 样本噪声表；同一声部的 4 个 voice 共享同一张表，
   这与 Java 里用相同种子构造 4 个 Voice 的结果完全一致（表内容相同）。 */
const NOISE_TABLES = [];

function noiseTable(pad) {
    if (!NOISE_TABLES[pad]) {
        const rnd = new JavaRandom(0x808 + pad * 7919);   // 与 Java 的种子表达式一致
        const table = new Float32Array(8192);
        for (let i = 0; i < table.length; i++) table[i] = rnd.nextDouble() * 2 - 1;
        NOISE_TABLES[pad] = table;
    }
    return NOISE_TABLES[pad];
}

/* 小工具：Java 里的 XuDsp.clamp */
function clamp(x, lo, hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ----------------------------------------------------------------------------
 * Biquad：RBJ Audio EQ Cookbook 双二阶，系数更新带缓存
 * 关键点：setXxx 在每个样本都会被调用（滤波器系数跟随参数），
 * 但参数未变时必须立刻 return，否则每样本两次 sin/cos 会把 CPU 吃光。
 * -------------------------------------------------------------------------- */
class Biquad {
    constructor() {
        this.b0 = 1;
        this.b1 = 0;
        this.b2 = 0;
        this.a1 = 0;
        this.a2 = 0;
        this.z1 = 0;
        this.z2 = 0;     // 直接 II 型的两个状态
        this.kind = 0;                // 0=未设置 1=LP 2=HP 3=BP
        this.fc = 0;
        this.q = 1;      // 缓存上次系数，用于跳过重复计算
    }

    setLowPass(f, q) {
        f = clamp(f, 20, SR * 0.49);           // 绝不允许超过奈奎斯特
        q = clamp(q, 0.1, 20);
        if (this.kind === 1 && Math.abs(f - this.fc) < 0.5 && Math.abs(q - this.q) < 1e-3) return;
        const w0 = TWO_PI * f / SR, cw = Math.cos(w0);
        const alpha = Math.sin(w0) / (2 * q), a0 = 1 + alpha;
        this.b0 = (1 - cw) * 0.5 / a0;
        this.b1 = (1 - cw) / a0;
        this.b2 = this.b0;
        this.a1 = -2 * cw / a0;
        this.a2 = (1 - alpha) / a0;
        this.kind = 1;
        this.fc = f;
        this.q = q;
    }

    setHighPass(f, q) {
        f = clamp(f, 10, SR * 0.49);
        q = clamp(q, 0.1, 20);
        if (this.kind === 2 && Math.abs(f - this.fc) < 0.5 && Math.abs(q - this.q) < 1e-3) return;
        const w0 = TWO_PI * f / SR, cw = Math.cos(w0);
        const alpha = Math.sin(w0) / (2 * q), a0 = 1 + alpha;
        this.b0 = (1 + cw) * 0.5 / a0;
        this.b1 = -(1 + cw) / a0;
        this.b2 = this.b0;
        this.a1 = -2 * cw / a0;
        this.a2 = (1 - alpha) / a0;
        this.kind = 2;
        this.fc = f;
        this.q = q;
    }

    setBandPass(f, q) {
        f = clamp(f, 20, SR * 0.49);
        q = clamp(q, 0.1, 30);
        if (this.kind === 3 && Math.abs(f - this.fc) < 0.5 && Math.abs(q - this.q) < 1e-3) return;
        const w0 = TWO_PI * f / SR, cw = Math.cos(w0);
        const alpha = Math.sin(w0) / (2 * q), a0 = 1 + alpha;
        this.b0 = alpha / a0;
        this.b1 = 0;
        this.b2 = -alpha / a0;
        this.a1 = -2 * cw / a0;
        this.a2 = (1 - alpha) / a0;
        this.kind = 3;
        this.fc = f;
        this.q = q;
    }

    run(x) {
        const y = this.b0 * x + this.z1;
        this.z1 = this.b1 * x - this.a1 * y + this.z2;
        this.z2 = this.b2 * x - this.a2 * y;
        return y;
    }

    reset() {
        this.z1 = 0;
        this.z2 = 0;
    }
}

/* ----------------------------------------------------------------------------
 * Voice：一个发声单元。所有状态在构造时分配，render 内不做任何 new。
 * 16 声部 × 4 voice = 64 个对象，整个生命周期只创建一次。
 * -------------------------------------------------------------------------- */
class Voice {
    constructor(pad) {
        this.pad = pad;
        this.active = false;
        this.start = 0;
        this.stop = Infinity;
        this.gain = 1;
        this.accent = false;
        this.fadeLen = 64;
        this.noise = noiseTable(pad);   // 共享噪声表，省内存也保证与 Java 一致
        this.npos = 0;
        this.bp = new Biquad();
        this.hp = new Biquad();
        this.bp2 = new Biquad();
    }

    static square(phase) {
        return Math.sin(phase) >= 0 ? 1 : -1;
    }

    /** 与 XuDsp.Voice.trigger 对应：重置噪声指针与滤波器状态 */
    trigger(when, gain, accent) {
        this.active = true;
        this.start = when;
        this.stop = Infinity;
        this.gain = gain;
        this.accent = accent;
        this.fadeLen = 64;
        this.npos = 0;
        this.bp.reset();
        this.hp.reset();
        this.bp2.reset();
    }

    /** 与 XuDsp.Voice.kill 对应：安排一个短淡出，避免硬切爆音 */
    kill(when, fade) {
        if (!this.active) return;
        this.stop = Math.min(this.stop, when);
        this.fadeLen = Math.max(1, fade);
    }

    /** 循环读取噪声表，和 Java 的 npos 回绕完全一样 */
    noiseSample() {
        if (this.npos >= this.noise.length) this.npos = 0;
        return this.noise[this.npos++];
    }

    /**
     * 把一个时间区间 [a,b) 叠加进左右声道。
     * b0 是本块的绝对起点，用于把绝对样本号换算成数组下标。
     */
    render(L, R, n, b0, b1, p) {
        if (!this.active) return;
        const a = Math.max(b0, this.start);
        const b = Math.min(b1, this.stop);
        if (b <= a) {
            if (b1 > this.stop) this.active = false;   // 淡出结束后释放 voice
            return;
        }
        const m = Math.floor(b - a);
        const off = Math.floor(a - b0);
        if (m <= 0 || off >= n) return;

        const pan = clamp(p.pan, 0, 1);
        const lg = Math.cos(pan * Math.PI * 0.5);
        const rg = Math.sin(pan * Math.PI * 0.5);
        const g = this.gain * Math.max(0, p.level) * 0.42;   // 0.42 是 Java 版的混音余量

        for (let i = 0; i < m; i++) {
            const abs = a + i;
            const t = (abs - this.start) / SR;
            let s = this.synth(t, p);
            if (this.stop < Infinity) {
                const dist = this.stop - abs;
                if (dist < this.fadeLen) s *= dist <= 0 ? 0 : dist / this.fadeLen;
            }
            L[off + i] += s * g * lg;
            R[off + i] += s * g * rg;
        }
        // 规格：任何 voice 最多存活 4 秒，杜绝“上个音不关越叠越多”
        if (b1 - this.start > 4 * SR) this.active = false;
    }

    /** 逐样本合成；这里是 XuDsp.synth 的逐行移植，公式一个都不改 */
    synth(t, p) {
        switch (this.pad) {
            case 0: { // BD 底鼓：桥接 T 型正弦 + 音高包络 + 点击噪声
                const fEnd = 40 + p.tune * 40;
                const fStart = fEnd * (1.6 + p.tone * 1.9);
                const tau = 0.018 + (1 - p.decay) * 0.012;
                const df = fStart - fEnd;
                const phase = TWO_PI * (fEnd * t + df * tau * (1 - Math.exp(-t / tau)));
                const dec = (0.12 + p.decay * 1.48) * (this.accent ? 1.18 : 1);
                const amp = Math.exp(-t / dec);
                const n = this.noiseSample();
                this.hp.setHighPass(1800 + p.tone * 3000, 0.8);
                const click = this.hp.run(n) * Math.exp(-t / 0.0035) * (0.06 + p.tone * 0.32);
                const body = Math.tanh(Math.sin(phase) * (1.15 + p.decay * 1.2)) * 0.92;
                return body * amp + click;
            }
            case 1: { // SD 军鼓：双正弦 150–260Hz + 带通噪声
                const f1 = 150 + p.tune * 110;
                const f2 = f1 * (1.78 + p.tone * 0.22);
                const decO = 0.055 + p.decay * 0.14;
                const osc = 0.55 * Math.sin(TWO_PI * f1 * t) * Math.exp(-t / decO)
                    + 0.34 * Math.sin(TWO_PI * f2 * t) * Math.exp(-t / (decO * 0.72));
                const n = this.noiseSample();
                this.bp.setBandPass(1200 + p.snappy * 3800, 0.9 + p.tone * 0.8);
                let nz = this.bp.run(n);
                const decN = (0.075 + p.decay * 0.16) * (this.accent ? 0.85 : 1);
                nz *= Math.exp(-t / decN) * (0.25 + p.snappy * 0.7);
                return osc * 0.7 + nz;
            }
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7: { // 嗵鼓与康加
                let baseF, decayS, clickAmt, sweep;
                if (this.pad <= 4) {
                    baseF = this.pad === 2 ? 80 : this.pad === 3 ? 120 : 170;
                    decayS = 0.15 + p.decay * 0.75;
                    clickAmt = 0.04 + p.tone * 0.22;
                    sweep = 1.9;
                } else {
                    baseF = this.pad === 5 ? 220 : this.pad === 6 ? 300 : 400;
                    decayS = 0.10 + p.decay * 0.55;
                    clickAmt = 0.02 + p.tone * 0.12;
                    sweep = 1.35;
                }
                const f0 = baseF * (0.6 + p.tune);
                const tau = this.pad <= 4 ? 0.035 : 0.02;
                const phase = TWO_PI * (f0 * t + f0 * (sweep - 1) * tau * (1 - Math.exp(-t / tau)));
                const amp = Math.exp(-t / decayS);
                const n = this.noiseSample();
                this.hp.setHighPass(2500, 0.7);
                const click = this.hp.run(n) * Math.exp(-t / 0.004) * clickAmt;
                return Math.sin(phase) * amp * 0.9 + click;
            }
            case 8: { // RS 边击：1.2–2.6kHz 方波经带通 + 高频点击
                const f = 1200 + p.tune * 1400;
                const dec = 0.012 + p.decay * 0.05;
                const sq = Voice.square(TWO_PI * f * t);
                this.bp.setBandPass(f, 2.2);
                const body = this.bp.run(sq) * Math.exp(-t / dec);
                const n = this.noiseSample();
                this.hp.setHighPass(3000, 0.8);
                const tick = this.hp.run(n) * Math.exp(-t / 0.004) * 0.35;
                return body * 0.8 + tick;
            }
            case 9: { // CL 击木：2.0–3.0kHz 正弦，30ms 量级
                const f = 2000 + p.tune * 1000;
                const dec = 0.018 + p.decay * 0.035;
                return Math.sin(TWO_PI * f * t) * Math.exp(-t / dec) * 0.9;
            }
            case 10: { // CP 拍手：4 段噪声爆发 + 尾巴
                const n = this.noiseSample();
                this.bp.setBandPass(800 + p.tone * 1700, 1.1);
                const nz = this.bp.run(n);
                let env = 0;
                for (let k = 0; k < 4; k++) {
                    const tk = k * 0.009;
                    if (t >= tk) env += Math.exp(-(t - tk) / 0.011) * 0.55;
                }
                const tail = Math.exp(-t / (0.05 + p.decay * 0.35)) * (0.15 + p.tail * 0.55);
                return nz * (env + tail);
            }
            case 11: { // MA 沙锤：高通噪声
                const n = this.noiseSample();
                this.hp.setHighPass(4000 + p.tone * 5000, 0.9);
                const nz = this.hp.run(n);
                const env = (1 - Math.exp(-t / 0.0015)) * Math.exp(-t / (0.018 + p.decay * 0.12));
                return nz * env * 0.85;
            }
            case 12: { // CB 牛铃：540/800Hz 双方波 + 带通
                const f = 400 + p.tune * 300;
                const sq1 = Voice.square(TWO_PI * f * t);
                const sq2 = Voice.square(TWO_PI * f * 1.48 * t);
                this.bp.setBandPass(2400 + p.tone * 1200, 1.4);
                const body = this.bp.run(sq1 * 0.5 + sq2 * 0.5);
                const dec = 0.05 + p.decay * 0.35;
                return body * Math.exp(-t / dec) * 0.85;
            }
            default: { // CY / OH / CH：6 方波金属簇 + 高通 + 带通
                const ratios = [1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523];
                const root = 320 * (0.75 + p.tune * 0.5);
                let metal = 0;
                for (let i = 0; i < ratios.length; i++) metal += Voice.square(TWO_PI * root * ratios[i] * t);
                metal /= ratios.length;
                this.hp.setHighPass(5200 + p.tone * 2600, 0.8);
                metal = this.hp.run(metal);
                this.bp.setBandPass(7600 + p.tone * 3600, 0.9);
                metal = this.bp.run(metal);
                const dec = this.pad === 15 ? 0.028 + p.decay * 0.20
                    : this.pad === 14 ? 0.12 + p.decay * 0.65
                        : 0.25 + p.decay * 1.75;
                const env = Math.exp(-t / dec);
                let out = metal * env;
                if (this.pad === 13) {   // 吊镲额外混一点 10kHz 噪声，增加空气感
                    const n = this.noiseSample();
                    this.bp2.setBandPass(10000, 0.7);
                    out += this.bp2.run(n) * env * 0.12;
                }
                return out * 0.75;
            }
        }
    }
}

/* ----------------------------------------------------------------------------
 * Engine：完全对应 XuEngine 的实时音序与混音
 * 事件队列用四条平行数组（时刻/声部/力度/重音），容量 4096，满了丢最旧。
 * 这样 process() 里不会产生任何临时对象，V8 的 GC 不会来打扰节拍。
 * -------------------------------------------------------------------------- */
class Engine {
    constructor() {
        this.proj = null;
        this.playing = false;
        this.samplePos = 0;
        this.nextStepAbs = 0;
        this.stepIndex = 0;
        this.globalStep = 0;
        this.cycleCount = 0;
        this.fillNow = false;
        this.pendingPattern = -1;
        this.songIndex = 0;
        this.songRepeat = 0;
        this.rng = 0x808808;
        this.soloOverride = -1;
        this.meterL = 0;
        this.meterR = 0;
        this.uiStep = -1;          // 给 UI 的播放头列号（正在发声的这一步）
        this.uiPattern = 0;

        // 16 声部 × 4 voice，一次性分配
        this.voices = [];
        for (let pad = 0; pad < 16; pad++) {
            const pool = [];
            for (let v = 0; v < 4; v++) pool.push(new Voice(pad));
            this.voices.push(pool);
        }

        const MAX = 4096;
        // p* = 音序器产出的事件；m* = 手动试听事件；b* = 本块待渲染事件
        this.pT = new Float64Array(MAX);
        this.pPad = new Int32Array(MAX);
        this.pVel = new Float32Array(MAX);
        this.pAcc = new Uint8Array(MAX);
        this.pCount = 0;
        this.mT = new Float64Array(MAX);
        this.mPad = new Int32Array(MAX);
        this.mVel = new Float32Array(MAX);
        this.mAcc = new Uint8Array(MAX);
        this.mCount = 0;
        this.bT = new Float64Array(MAX);
        this.bPad = new Int32Array(MAX);
        this.bVel = new Float32Array(MAX);
        this.bAcc = new Uint8Array(MAX);
        this.bCount = 0;

        this.lpL = new Biquad();
        this.lpR = new Biquad();
        this.hpL = new Biquad();
        this.hpR = new Biquad();
    }

    /* ------------------------- 工程与参数同步 ------------------------- */
    setProject(j) {
        this.proj = j;
        this.rng = (j.seed && j.seed !== 0) ? j.seed : 0x808808;   // 与 Java 构造器一致
        this.stop();                     // 换工程先停，避免旧 pattern 的 voice 残留
        // 徐某：载入新工程时还要硬清 voice 与三个事件队列。
        // 否则旧工程的 start 时刻会落在新时间轴之外，t 变成负数，包络会算出怪值。
        for (let pad = 0; pad < 16; pad++) {
            const pool = this.voices[pad];
            for (let v = 0; v < pool.length; v++) {
                pool[v].active = false;
                pool[v].stop = Infinity;
                pool[v].npos = 0;
                pool[v].bp.reset();
                pool[v].hp.reset();
                pool[v].bp2.reset();
            }
        }
        this.pCount = 0;
        this.mCount = 0;
        this.bCount = 0;
        this.samplePos = 0;              // 离线/实时都从 0 开始，导出与试听时间轴一致
        this.pendingPattern = -1;
        this.songIndex = 0;
        this.songRepeat = 0;
        this.cycleCount = 0;
        this.fillNow = false;
        this.uiStep = -1;
    }

    /** 增量补丁：UI 只发变化的那一小块，避免每次点击都传 1MB 工程 */
    apply(msg) {
        if (!this.proj) return;
        switch (msg.type) {
            case 'pad': {
                const p = this.proj.pads[msg.pad];
                if (p && msg.key in p) p[msg.key] = msg.value;
                break;
            }
            case 'master': {
                if (msg.key in this.proj.master) this.proj.master[msg.key] = msg.value;
                break;
            }
            case 'step': {
                const pat = this.proj.patterns[msg.pattern];
                if (pat && pat.data[msg.pad]) pat.data[msg.pad][msg.step] = msg.data;
                break;
            }
            case 'steps': {
                const pat = this.proj.patterns[this.proj.current];
                pat.steps = clamp(msg.value | 0, 1, 64);
                break;
            }
            case 'trackLen': {
                const pat = this.proj.patterns[msg.pattern];
                if (pat) pat.trackLen[msg.pad] = clamp(msg.value | 0, 1, 64);
                break;
            }
            case 'pattern': {
                this.requestPattern(msg.index | 0);
                break;
            }
            case 'song': {
                this.proj.master.songMode = !!msg.enabled;
                this.proj.song.entries = msg.entries;
                break;
            }
            case 'project': {
                this.setProject(msg.project);
                break;
            }
            case 'play': {
                this.play();
                break;
            }
            case 'stop': {
                this.stop();
                break;
            }
            case 'trigger': {
                this.triggerPad(msg.pad | 0, msg.vel | 0, !!msg.accent);
                break;
            }
            default:
                break;   // 未知消息直接忽略，保证向前兼容
        }
    }

    /* ------------------------- 传输 ------------------------- */
    play() {
        if (this.playing) return;
        this.playing = true;
        this.stepIndex = 0;
        this.globalStep = 0;
        this.cycleCount = 0;
        this.fillNow = false;
        this.songIndex = 0;
        this.songRepeat = 0;
        this.nextStepAbs = this.samplePos;
        this.pCount = 0;
        this.pendingPattern = -1;
    }

    stop() {
        this.playing = false;
        // 规格：停止要自然收尾，480 样本淡出约 10ms，避免“啪”的爆音
        for (let pad = 0; pad < 16; pad++) {
            const pool = this.voices[pad];
            for (let v = 0; v < pool.length; v++) pool[v].kill(this.samplePos, 480);
        }
        this.pCount = 0;
        this.uiStep = -1;
    }

    stepLen() {
        const bpm = Math.max(40, Math.min(300, this.proj.master.bpm));
        return Math.max(64, Math.round(SR * 60 / bpm / 4));
    }

    /** xorshift32：音频线程内的确定性随机，保证概率触发可复现 */
    nextRand() {
        let x = this.rng;
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        this.rng = x | 0;
        return this.rng;
    }

    condOk(cond) {
        switch (cond) {
            case 0:
                return true;
            case 1:
                return this.fillNow;
            case 2:
                return !this.fillNow;
            case 3:
                return (this.cycleCount % 2) === 0;
            case 4:
                return (this.cycleCount % 4) === 0;
            default:
                return true;
        }
    }

    /* ------------------------- 事件队列 ------------------------- */
    push(t, pad, vel, accent) {
        if (this.pCount >= 4096) {
            // 队列满时整体左移，丢弃最旧事件；绝不扩容，保证内存有界
            this.pT.copyWithin(0, 1);
            this.pPad.copyWithin(0, 1);
            this.pVel.copyWithin(0, 1);
            this.pAcc.copyWithin(0, 1);
            this.pCount--;
        }
        const i = this.pCount++;
        this.pT[i] = t;
        this.pPad[i] = pad;
        this.pVel[i] = vel;
        this.pAcc[i] = accent ? 1 : 0;
    }

    /**
     * 展开当前步：swing、概率、微时、ratchet、重音全部按样本落位。
     * stepAbs 是这一步“未摇摆”的理论起点。
     */
    emitStep(stepAbs) {
        const pat = this.proj.patterns[this.proj.current];
        const sl = this.stepLen();
        const swingOff = (this.stepIndex & 1) ? Math.trunc(sl * this.proj.master.swing * 0.5) : 0;
        const base = stepAbs + swingOff;
        this.uiStep = this.stepIndex;            // UI 播放头与实际发声同步
        this.uiPattern = this.proj.current;

        for (let pad = 0; pad < 16; pad++) {
            const tl = Math.max(1, Math.min(64, pat.trackLen[pad]));
            const idx = this.globalStep % tl;        // polymeter：轨道用自己的长度取模
            const st = pat.data[pad][idx];
            if (!st || !st[0] || !this.condOk(st[6]) || st[3] <= 0) continue;
            if (st[3] < 100 && (this.nextRand() % 100) >= st[3]) continue;

            let vel = Math.max(0.02, Math.min(1, st[1] / 127));
            if (st[2]) vel = Math.min(1.25, vel * 1.30);      // 重音在合成层再加 30%
            const r = Math.max(1, Math.min(8, st[5]));
            for (let i = 0; i < r; i++) {
                const t = base + st[4] + (i * sl) / r;           // 微时以样本为单位直接叠加
                this.push(t, pad, vel * Math.pow(0.92, i), !!st[2]);
            }
        }
    }

    advanceAfterStep() {
        const pat = this.proj.patterns[this.proj.current];
        this.stepIndex++;
        this.globalStep++;
        if (this.stepIndex < Math.max(1, pat.steps)) return;
        this.stepIndex = 0;
        this.cycleCount++;
        this.fillNow = (this.cycleCount % 4) === 3;   // 每 4 循环的最后一轮当“过门”

        if (this.pendingPattern >= 0) {
            // 规格：Pattern 切换在下一步边界生效
            this.proj.current = Math.max(0, Math.min(15, this.pendingPattern));
            this.pendingPattern = -1;
            this.songRepeat = 0;
            return;
        }
        const song = this.proj.song;
        if (this.proj.master.songMode && song && song.entries && song.entries.length) {
            const en = song.entries[this.songIndex % song.entries.length];
            this.songRepeat++;
            if (this.songRepeat >= Math.max(1, en.repeats)) {
                this.songRepeat = 0;
                this.songIndex = (this.songIndex + 1) % song.entries.length;
                this.proj.current = Math.max(0, Math.min(15, song.entries[this.songIndex].pattern));
            }
        }
    }

    /** Mute/Solo 判断：solo 优先于 mute，与 Java 版一致 */
    audible(pad) {
        if (this.soloOverride >= 0) return pad === this.soloOverride;
        let anySolo = false;
        for (let i = 0; i < 16; i++) if (this.proj.pads[i].solo) {
            anySolo = true;
            break;
        }
        const p = this.proj.pads[pad];
        if (p.mute) return false;
        if (anySolo && !p.solo) return false;
        return true;
    }

    /** 触发一个声部：开/闭镲先掐断开镲，再按“取空闲 voice，否则抢最老”分配 */
    trigger(pad, when, vel, accent) {
        if (pad === 15 || pad === 14) {
            const oh = this.voices[14];
            for (let v = 0; v < oh.length; v++) oh[v].kill(when, 16);
        }
        const pool = this.voices[pad];
        let target = null;
        for (let v = 0; v < pool.length; v++) {
            if (!pool[v].active) {
                target = pool[v];
                break;
            }
        }
        if (!target) {
            target = pool[0];
            for (let v = 0; v < pool.length; v++) if (pool[v].start < target.start) target = pool[v];
            target.kill(when, 64);   // 抢占也要淡出，防止爆音
        }
        target.trigger(when, vel, accent);
    }

    /* ------------------------- 渲染 ------------------------- */
    render(n, L, R) {
        const b0 = this.samplePos, b1 = b0 + n;
        L.fill(0, 0, n);
        R.fill(0, 0, n);
        this.collect(b0, b1);

        // 按事件时刻把块切成若干段，段内 voice 连续合成，段边界才新触发
        let cur = b0;
        for (let i = 0; i < this.bCount; i++) {
            const t = this.bT[i];
            if (t > cur) {
                this.renderVoices(L, R, n, b0, cur, t);
                cur = t;
            }
            if (t < b1) this.trigger(this.bPad[i], t, this.bVel[i], !!this.bAcc[i]);
        }
        if (cur < b1) this.renderVoices(L, R, n, b0, cur, b1);

        const m = this.proj.master;
        // 效果链顺序固定：HP → LP → Drive → tanh 软限幅 → 主音量
        if (m.hpOn) {
            this.hpL.setHighPass(m.hpCutoff, m.hpQ);
            this.hpR.setHighPass(m.hpCutoff, m.hpQ);
            for (let i = 0; i < n; i++) {
                L[i] = this.hpL.run(L[i]);
                R[i] = this.hpR.run(R[i]);
            }
        }
        if (m.lpOn) {
            this.lpL.setLowPass(m.lpCutoff, m.lpQ);
            this.lpR.setLowPass(m.lpCutoff, m.lpQ);
            for (let i = 0; i < n; i++) {
                L[i] = this.lpL.run(L[i]);
                R[i] = this.lpR.run(R[i]);
            }
        }
        const drive = Math.max(1, Math.min(8, m.drive));
        const norm = Math.tanh(drive);
        const vol = Math.max(0, Math.min(1, m.volume));
        let pl = 0, pr = 0;
        for (let i = 0; i < n; i++) {
            let xl = L[i], xr = R[i];
            if (drive > 1.001) {
                xl = Math.tanh(xl * drive) / norm;
                xr = Math.tanh(xr * drive) / norm;
            }
            xl = xl / (1 + Math.abs(xl));      // 软限幅，永远不会超过 ±1
            xr = xr / (1 + Math.abs(xr));
            xl *= vol;
            xr *= vol;
            L[i] = xl;
            R[i] = xr;
            if (Math.abs(xl) > pl) pl = Math.abs(xl);
            if (Math.abs(xr) > pr) pr = Math.abs(xr);
        }
        // 电平表带一点保持衰减，视觉上不会闪成噪点
        this.meterL = Math.max(pl, this.meterL * 0.75);
        this.meterR = Math.max(pr, this.meterR * 0.75);
        this.samplePos = b1;

        // 兜底回收：即使 stop 漏了，4 秒后也强制释放
        for (let pad = 0; pad < 16; pad++) {
            const pool = this.voices[pad];
            for (let v = 0; v < pool.length; v++) {
                if (pool[v].active && b0 - pool[v].start > 4 * SR) pool[v].active = false;
            }
        }
    }

    /** 把到期的 step 事件与手动事件合并进本块缓冲，并生成新的 step */
    collect(b0, b1) {
        this.bCount = 0;

        // 1) 手动试听事件
        let keep = 0;
        for (let i = 0; i < this.mCount; i++) {
            const t = this.mT[i];
            if (t < b1) {
                this.addBlock(Math.max(t, b0), this.mPad[i], this.mVel[i], this.mAcc[i] !== 0);
            } else {
                this.mT[keep] = t;
                this.mPad[keep] = this.mPad[i];
                this.mVel[keep] = this.mVel[i];
                this.mAcc[keep] = this.mAcc[i];
                keep++;
            }
        }
        this.mCount = keep;

        // 2) 音频线程主时钟推进：到点就展开一步
        if (this.playing && this.proj) {
            let guard = 0;
            while (this.nextStepAbs < b1 && guard++ < 64) {
                this.emitStep(Math.max(this.nextStepAbs, b0));
                this.nextStepAbs += this.stepLen();
                this.advanceAfterStep();
            }
        }

        // 3) 音序事件
        keep = 0;
        for (let i = 0; i < this.pCount; i++) {
            const t = this.pT[i];
            if (t < b1) {
                this.addBlock(Math.max(t, b0), this.pPad[i], this.pVel[i], this.pAcc[i] !== 0);
            } else {
                this.pT[keep] = t;
                this.pPad[keep] = this.pPad[i];
                this.pVel[keep] = this.pVel[i];
                this.pAcc[keep] = this.pAcc[i];
                keep++;
            }
        }
        this.pCount = keep;

        // 4) 插入排序：事件数通常个位数，比通用排序更省，也能同步四条数组
        for (let i = 1; i < this.bCount; i++) {
            const t = this.bT[i], pd = this.bPad[i], vv = this.bVel[i], ac = this.bAcc[i];
            let j = i - 1;
            while (j >= 0 && this.bT[j] > t) {
                this.bT[j + 1] = this.bT[j];
                this.bPad[j + 1] = this.bPad[j];
                this.bVel[j + 1] = this.bVel[j];
                this.bAcc[j + 1] = this.bAcc[j];
                j--;
            }
            this.bT[j + 1] = t;
            this.bPad[j + 1] = pd;
            this.bVel[j + 1] = vv;
            this.bAcc[j + 1] = ac;
        }
    }

    addBlock(t, pad, vel, accent) {
        if (this.bCount >= 4096) return;
        const i = this.bCount++;
        this.bT[i] = t;
        this.bPad[i] = pad;
        this.bVel[i] = vel;
        this.bAcc[i] = accent ? 1 : 0;
    }

    /** 在 [a,b) 区间内把所有可听声部的活跃 voice 叠加到总线 */
    renderVoices(L, R, n, b0, a, b) {
        if (b <= a) return;
        for (let pad = 0; pad < 16; pad++) {
            if (!this.audible(pad)) continue;      // Mute/Solo 在混音阶段生效
            const p = this.proj.pads[pad];
            const pool = this.voices[pad];
            for (let v = 0; v < pool.length; v++) {
                if (pool[v].active) pool[v].render(L, R, n, b0, b, p);
            }
        }
    }

    /** 手动试听：延迟 32 样本（约 0.7ms）让事件排到当前块之后，避免“过去触发” */
    triggerPad(pad, vel, accent) {
        if (!this.proj || pad < 0 || pad >= 16) return;
        vel = Math.max(1, Math.min(127, vel));
        if (this.mCount >= 4096) return;
        const i = this.mCount++;
        this.mT[i] = this.samplePos + 32;
        this.mPad[i] = pad;
        this.mVel[i] = vel / 127;
        this.mAcc[i] = accent ? 1 : 0;
    }

    /** 播放中请求切换 Pattern：只登记，下一步边界才真正换 */
    requestPattern(idx) {
        idx = Math.max(0, Math.min(15, idx));
        if (!this.proj) return;
        if (this.playing) this.pendingPattern = idx;
        else this.proj.current = idx;
    }
}

/* ----------------------------------------------------------------------------
 * Xu808Processor：AudioWorklet 入口
 * process() 每 128 帧调用一次，绝不分配大对象，只做定长数组运算。
 * -------------------------------------------------------------------------- */
class Xu808Processor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.engine = new Engine();
        this.blocks = 0;
        this.engine.setProject(Xu808Processor.emptyProject());  // 先给一个合法空工程，防止首帧空指针
        this.port.onmessage = (e) => this.onMessage(e.data);
        this.port.postMessage({type: 'ready', sampleRate: SR});
    }

    /** 与 XuProject 默认结构一致的最小工程，真实工程由 Controller 注入页面后再发过来 */
    static emptyProject() {
        const patterns = [];
        for (let i = 0; i < 16; i++) {
            const data = [];
            for (let p = 0; p < 16; p++) {
                const row = [];
                for (let s = 0; s < 64; s++) row.push([0, 100, 0, 100, 0, 1, 0]);
                data.push(row);
            }
            patterns.push({name: 'PATTERN ' + (i + 1), steps: 16, fill: false, trackLen: new Array(16).fill(16), data});
        }
        const pads = [];
        for (let i = 0; i < 16; i++) {
            pads.push({
                tune: 0.5,
                decay: 0.5,
                tone: 0.5,
                snappy: 0.55,
                tail: 0.45,
                level: 0.75,
                pan: 0.5,
                mute: false,
                solo: false
            });
        }
        return {
            format: 'XU808', version: 1, name: 'XU-808', seed: 0x808808, current: 0,
            master: {
                volume: 0.8, drive: 1.35, lpCutoff: 18000, lpQ: 0.9, hpCutoff: 28, hpQ: 0.7,
                lpOn: true, hpOn: true, bpm: 120, swing: 0, beatsPerBar: 4, beatUnit: 4, songMode: false
            },
            pads, patterns, song: {enabled: false, entries: [{pattern: 0, repeats: 4}]}
        };
    }

    onMessage(msg) {
        if (!msg || !msg.type) return;
        try {
            if (msg.type === 'project') {
                this.engine.setProject(msg.project);
            } else {
                this.engine.apply(msg);
            }
            // 每次状态变化回一个 ack，UI 可据此确认“参数已进音频线程”
            if (msg.type === 'play' || msg.type === 'stop' || msg.type === 'project') {
                this.port.postMessage({type: 'ack', action: msg.type, playing: this.engine.playing});
            }
        } catch (err) {
            // worklet 里没法弹窗，只能把错误送回主线程显示在状态栏
            this.port.postMessage({type: 'error', message: String(err && err.message ? err.message : err)});
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || !out[0]) return true;
        const L = out[0];
        const R = out.length > 1 ? out[1] : out[0];
        const n = L.length;

        this.engine.render(n, L, R);

        // 约每 8 个量子（128×8/48000 ≈ 21ms）回传一次状态，足够 60fps 的播放头
        this.blocks++;
        if (this.blocks >= 8) {
            this.blocks = 0;
            const e = this.engine;
            this.port.postMessage({
                type: 'pos',
                step: e.uiStep,
                pattern: e.uiPattern,
                playing: e.playing,
                meterL: Math.min(1, e.meterL),
                meterR: Math.min(1, e.meterR)
            });
        }
        return true;   // 永远保持处理器存活
    }
}

registerProcessor('xu808-engine', Xu808Processor);
