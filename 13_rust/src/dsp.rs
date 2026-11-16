// ============================================================================
//  dsp.rs —— 十六声部模拟鼓合成（作者：徐某）
//
//  这是从纯 C 版 dsp.c 逐行重写的版本，公式、系数、魔法数字全部保持一致，
//  这样同一个工程文件在 C 版和 Rust 版里听起来是同一台鼓机。
//
//  实时纪律（音频回调里绝不做的事）：
//    · 不 malloc/free —— 噪声池、滤波器状态都在 Engine::new 时一次性建好；
//    · 不加锁、不做文件 IO、不调用 UI；
//    · 每个样本只做 sin/exp/tanh 和几条乘加；
//    · 包络是“年龄 t”的解析函数，声音到时间自然归零，voice 用完即回收。
// ============================================================================
use crate::project::{PadParams, MAX_VOICES, PADS, SR};

/// RBJ 双二阶滤波器。z1/z2 必须跨块保留，否则每块开头都会“咔”一声。
pub struct Biquad {
    pub b0: f64,
    pub b1: f64,
    pub b2: f64,
    pub a1: f64,
    pub a2: f64,
    pub z1: f64,
    pub z2: f64,
    pub kind: i32, // 0=直通 1=LP 2=HP 3=BP
    pub fc: f64,
    pub q: f64,
}

impl Biquad {
    pub const fn new() -> Biquad {
        Biquad { b0: 1.0, b1: 0.0, b2: 0.0, a1: 0.0, a2: 0.0, z1: 0.0, z2: 0.0, kind: 0, fc: 0.0, q: 0.0 }
    }

    /// 清空延迟线（触发新 voice 时调用，避免上一槌的残余造成“噗”）
    pub fn reset(&mut self) {
        self.z1 = 0.0;
        self.z2 = 0.0;
    }

    /// RBJ 低通
    pub fn set_lp(&mut self, fc: f64, q: f64) {
        let fc = clampd(fc, 20.0, SR as f64 * 0.49);
        let q = clampd(q, 0.1, 20.0);
        if self.kind == 1 && (fc - self.fc).abs() < 0.5 && (q - self.q).abs() < 1e-3 {
            return;
        }
        let w0 = 2.0 * PI * fc / SR as f64;
        let cw = w0.cos();
        let alpha = w0.sin() / (2.0 * q);
        let a0 = 1.0 + alpha;
        self.b0 = (1.0 - cw) * 0.5 / a0;
        self.b1 = (1.0 - cw) / a0;
        self.b2 = self.b0;
        self.a1 = (-2.0 * cw) / a0;
        self.a2 = (1.0 - alpha) / a0;
        self.kind = 1;
        self.fc = fc;
        self.q = q;
    }

    /// RBJ 高通
    pub fn set_hp(&mut self, fc: f64, q: f64) {
        let fc = clampd(fc, 10.0, SR as f64 * 0.49);
        let q = clampd(q, 0.1, 20.0);
        if self.kind == 2 && (fc - self.fc).abs() < 0.5 && (q - self.q).abs() < 1e-3 {
            return;
        }
        let w0 = 2.0 * PI * fc / SR as f64;
        let cw = w0.cos();
        let alpha = w0.sin() / (2.0 * q);
        let a0 = 1.0 + alpha;
        self.b0 = (1.0 + cw) * 0.5 / a0;
        self.b1 = -(1.0 + cw) / a0;
        self.b2 = self.b0;
        self.a1 = (-2.0 * cw) / a0;
        self.a2 = (1.0 - alpha) / a0;
        self.kind = 2;
        self.fc = fc;
        self.q = q;
    }

    /// RBJ 带通（0dB 峰值）
    pub fn set_bp(&mut self, fc: f64, q: f64) {
        let fc = clampd(fc, 20.0, SR as f64 * 0.49);
        let q = clampd(q, 0.1, 30.0);
        if self.kind == 3 && (fc - self.fc).abs() < 0.5 && (q - self.q).abs() < 1e-3 {
            return;
        }
        let w0 = 2.0 * PI * fc / SR as f64;
        let cw = w0.cos();
        let alpha = w0.sin() / (2.0 * q);
        let a0 = 1.0 + alpha;
        self.b0 = alpha / a0;
        self.b1 = 0.0;
        self.b2 = -alpha / a0;
        self.a1 = (-2.0 * cw) / a0;
        self.a2 = (1.0 - alpha) / a0;
        self.kind = 3;
        self.fc = fc;
        self.q = q;
    }

    /// DF2T 差分方程
    #[inline]
    pub fn run(&mut self, x: f64) -> f64 {
        let y = self.b0 * x + self.z1;
        self.z1 = self.b1 * x - self.a1 * y + self.z2;
        self.z2 = self.b2 * x - self.a2 * y;
        y
    }
}

pub const PI: f64 = 3.141_592_653_589_793;

#[inline]
fn clampd(x: f64, lo: f64, hi: f64) -> f64 {
    if x < lo {
        lo
    } else if x > hi {
        hi
    } else {
        x
    }
}

/// 方波：sin >= 0 取 +1，否则 -1。比 signbit 更直观，和 C 版一致。
#[inline]
fn square(phase: f64) -> f64 {
    if phase.sin() >= 0.0 {
        1.0
    } else {
        -1.0
    }
}

/// 一个鼓槌（voice）。噪声池 8192 个 f32 = 32KB，触发时只用指针走位，不分配。
pub struct Voice {
    pub pad: usize,
    pub active: bool,
    pub start: i64, // 触发绝对样本
    pub stop: i64,  // 强制结束时刻（choke / stealing）
    pub gain: f32,  // 力度增益（含重音）
    pub accent: bool,
    pub fade_len: i32,
    pub noise: Vec<f32>,
    pub npos: usize,
    pub bp: Biquad,
    pub hp: Biquad,
    pub bp2: Biquad,
}

impl Voice {
    pub fn new(pad: usize) -> Voice {
        Voice {
            pad,
            active: false,
            start: 0,
            stop: i64::MAX,
            gain: 0.0,
            accent: false,
            fade_len: 64,
            noise: vec![0.0; 8192],
            npos: 0,
            bp: Biquad::new(),
            hp: Biquad::new(),
            bp2: Biquad::new(),
        }
    }

    /// 触发：设置起点、清滤波器记忆
    pub fn trigger(&mut self, when: i64, gain: f32, accent: bool) {
        self.active = true;
        self.start = when;
        self.stop = i64::MAX; // 默认永不强停
        self.gain = gain;
        self.accent = accent;
        self.fade_len = 64;
        self.npos = 0;
        self.bp.reset();
        self.hp.reset();
        self.bp2.reset();
    }

    /// 掐断：stop 必须比 when 晚 fade 个样本，淡出斜坡才有地方发生
    pub fn kill(&mut self, when: i64, fade: i32) {
        if !self.active {
            return;
        }
        let fade = fade.max(1) as i64;
        if when + fade < self.stop {
            self.stop = when + fade;
        }
        self.fade_len = fade as i32;
    }

    /// 从循环噪声池取一个样本（零分配）
    #[inline]
    fn take_noise(&mut self) -> f64 {
        if self.npos >= self.noise.len() {
            self.npos = 0;
        }
        let n = self.noise[self.npos] as f64;
        self.npos += 1;
        n
    }
}

// ============================================================================
//  核心合成函数：给定“年龄 t”（秒），返回该声部此刻的样本值。
//  pad 分派与所有公式必须与 C 版逐项对应。
// ============================================================================
fn synth_sample(v: &mut Voice, t: f64, p: &PadParams) -> f64 {
    match v.pad {
        // ================= 0 底鼓 =================
        0 => {
            let f_end = 40.0 + p.tune as f64 * 40.0;
            let f_start = f_end * (1.6 + p.tone as f64 * 1.9);
            let tau = 0.018 + (1.0 - p.decay as f64) * 0.012;
            let df = f_start - f_end;
            // 频率积分 → 相位：这是“鼓皮张力快速释放”的数学表达
            let phase = 2.0 * PI * (f_end * t + df * tau * (1.0 - (-t / tau).exp()));
            let dec = (0.12 + p.decay as f64 * 1.48) * if v.accent { 1.18 } else { 1.0 };
            let amp = (-t / dec).exp();
            let n = v.take_noise();
            v.hp.set_hp(1800.0 + p.tone as f64 * 3000.0, 0.8);
            let click = v.hp.run(n) * (-t / 0.0035).exp() * (0.06 + p.tone as f64 * 0.32);
            let body = (phase.sin() * (1.15 + p.decay as f64 * 1.2)).tanh() * 0.92;
            body * amp + click
        }

        // ================= 1 军鼓 =================
        1 => {
            let f1 = 150.0 + p.tune as f64 * 110.0;
            let f2 = f1 * (1.78 + p.tone as f64 * 0.22);
            let dec_o = 0.055 + p.decay as f64 * 0.14;
            let osc = 0.55 * (2.0 * PI * f1 * t).sin() * (-t / dec_o).exp()
                + 0.34 * (2.0 * PI * f2 * t).sin() * (-t / (dec_o * 0.72)).exp();
            let n = v.take_noise();
            v.bp.set_bp(1200.0 + p.snappy as f64 * 3800.0, 0.9 + p.tone as f64 * 0.8);
            let mut nz = v.bp.run(n);
            let dec_n = (0.075 + p.decay as f64 * 0.16) * if v.accent { 0.85 } else { 1.0 };
            nz *= (-t / dec_n).exp() * (0.25 + p.snappy as f64 * 0.7);
            osc * 0.7 + nz
        }

        // ================= 2..7 嗵鼓 / 康加 =================
        2..=7 => {
            let pad = v.pad;
            let (base_f, decay_s, click_amt, sweep) = if pad <= 4 {
                let bf = match pad {
                    2 => 80.0,
                    3 => 120.0,
                    _ => 170.0,
                };
                (bf, 0.15 + p.decay as f64 * 0.75, 0.04 + p.tone as f64 * 0.22, 1.9)
            } else {
                let bf = match pad {
                    5 => 220.0,
                    6 => 300.0,
                    _ => 400.0,
                };
                (bf, 0.10 + p.decay as f64 * 0.55, 0.02 + p.tone as f64 * 0.12, 1.35)
            };
            let f0 = base_f * (0.6 + p.tune as f64);
            let tau = if pad <= 4 { 0.035 } else { 0.020 };
            let phase = 2.0 * PI * (f0 * t + f0 * (sweep - 1.0) * tau * (1.0 - (-t / tau).exp()));
            let amp = (-t / decay_s).exp();
            let n = v.take_noise();
            v.hp.set_hp(2500.0, 0.7);
            let click = v.hp.run(n) * (-t / 0.004).exp() * click_amt;
            phase.sin() * amp * 0.9 + click
        }

        // ================= 8 边击 =================
        8 => {
            let f = 1200.0 + p.tune as f64 * 1400.0;
            let dec = 0.012 + p.decay as f64 * 0.05;
            let sq = square(2.0 * PI * f * t);
            v.bp.set_bp(f, 2.2);
            let body = v.bp.run(sq) * (-t / dec).exp();
            let n = v.take_noise();
            v.hp.set_hp(3000.0, 0.8);
            let tick = v.hp.run(n) * (-t / 0.004).exp() * 0.35;
            body * 0.8 + tick
        }

        // ================= 9 击木 =================
        9 => {
            let f = 2000.0 + p.tune as f64 * 1000.0;
            let dec = 0.018 + p.decay as f64 * 0.035;
            (2.0 * PI * f * t).sin() * (-t / dec).exp() * 0.9
        }

        // ================= 10 拍手 =================
        10 => {
            let n = v.take_noise();
            v.bp.set_bp(800.0 + p.tone as f64 * 1700.0, 1.1);
            let nz = v.bp.run(n);
            let mut env = 0.0;
            // 4 段噪声爆发：808 拍手的经典做法
            for k in 0..4 {
                let tk = k as f64 * 0.009;
                if t >= tk {
                    env += (-(t - tk) / 0.011).exp() * 0.55;
                }
            }
            let tail = (-t / (0.05 + p.decay as f64 * 0.35)).exp() * (0.15 + p.tail as f64 * 0.55);
            nz * (env + tail)
        }

        // ================= 11 沙锤 =================
        11 => {
            let n = v.take_noise();
            v.hp.set_hp(4000.0 + p.tone as f64 * 5000.0, 0.9);
            let nz = v.hp.run(n);
            let env = (1.0 - (-t / 0.0015).exp()) * (-t / (0.018 + p.decay as f64 * 0.12)).exp();
            nz * env * 0.85
        }

        // ================= 12 牛铃 =================
        12 => {
            let f = 400.0 + p.tune as f64 * 300.0;
            let sq1 = square(2.0 * PI * f * t);
            let sq2 = square(2.0 * PI * f * 1.48 * t);
            v.bp.set_bp(2400.0 + p.tone as f64 * 1200.0, 1.4);
            let body = v.bp.run(sq1 * 0.5 + sq2 * 0.5);
            let dec = 0.05 + p.decay as f64 * 0.35;
            body * (-t / dec).exp() * 0.85
        }

        // ================= 13..15 镲族 =================
        _ => {
            // 6 个非谐和方波叠加，是 808 金属声的核心技巧
            const RATIOS: [f64; 6] = [1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523];
            let root = 320.0 * (0.75 + p.tune as f64 * 0.5);
            let mut metal = 0.0;
            for r in RATIOS.iter() {
                metal += square(2.0 * PI * root * r * t);
            }
            metal /= 6.0;
            v.hp.set_hp(5200.0 + p.tone as f64 * 2600.0, 0.8);
            metal = v.hp.run(metal);
            v.bp.set_bp(7600.0 + p.tone as f64 * 3600.0, 0.9);
            metal = v.bp.run(metal);
            let dec = match v.pad {
                15 => 0.028 + p.decay as f64 * 0.20,
                14 => 0.12 + p.decay as f64 * 0.65,
                _ => 0.25 + p.decay as f64 * 1.75,
            };
            let env = (-t / dec).exp();
            let mut out = metal * env;
            if v.pad == 13 {
                let n = v.take_noise();
                v.bp2.set_bp(10000.0, 0.7);
                out += v.bp2.run(n) * env * 0.12;
            }
            out * 0.75
        }
    }
}

/// 把一个 voice 在 [b0,b1) 片段内渲染进左右声道。
/// block0 = 本块起点，seg_end = 本片段终点（绝对样本）。
pub fn render_voice(
    v: &mut Voice,
    l: &mut [f32; crate::project::BLOCK],
    r: &mut [f32; crate::project::BLOCK],
    block0: i64,
    seg_end: i64,
    p: &PadParams,
) {
    if !v.active {
        return;
    }
    let a = block0.max(v.start);
    let b = seg_end.min(v.stop);
    if b <= a {
        // 片段已经完全越过 stop，回收 voice
        if seg_end > v.stop {
            v.active = false;
        }
        return;
    }
    let m = (b - a) as usize;
    let off = (a - block0) as usize;
    if off >= l.len() {
        return;
    }
    let m = m.min(l.len() - off);

    // 等功率声像：pan=0.5 时左右各 0.707，总功率恒定
    let pan = clampd(p.pan as f64, 0.0, 1.0);
    let lg = (pan * PI * 0.5).cos();
    let rg = (pan * PI * 0.5).sin();
    let g = v.gain as f64 * if p.level > 0.0 { p.level as f64 } else { 0.0 } * 0.42;

    for i in 0..m {
        let abs = a + i as i64;
        let t = (abs - v.start) as f64 / SR as f64;
        let mut s = synth_sample(v, t, p);
        // 掐断淡出：距离 stop 越近，增益越小，避免硬切爆音
        if v.stop < i64::MAX {
            let dist = v.stop - abs;
            if dist < v.fade_len as i64 {
                s *= if dist <= 0 { 0.0 } else { dist as f64 / v.fade_len as f64 };
            }
        }
        l[off + i] += (s * g * lg) as f32;
        r[off + i] += (s * g * rg) as f32;
    }
    // 最多活 4 秒，到点无条件回收，保证不会无限堆叠
    if seg_end - v.start > 4 * SR as i64 {
        v.active = false;
    }
}

/// 16 个声部 × 4 个 voice 的一次性构造入口
pub fn make_voices() -> Vec<Vec<Voice>> {
    (0..PADS).map(|p| (0..MAX_VOICES).map(|_| Voice::new(p)).collect()).collect()
}
