// ============================================================================
//  engine.rs —— 音序器、混音、主效果、实时时钟（作者：徐某）
//
//  实时纪律在这里体现得最彻底：
//    1. sample_pos（i64）是唯一时钟，绝不用 UI 定时器打拍子；
//    2. 所有事件先展开成“绝对样本时刻”，再按块切段渲染；
//    3. 回调里不 malloc/free、不加锁、不读写文件；
//    4. 停止时给 voice 10ms 淡出，既即时安静又不“咔哒”；
//    5. GUI→音频只走无锁环形队列 + 原子变量，音频→GUI 只走原子快照。
// ============================================================================
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use crate::dsp::{make_voices, render_voice, Voice};
use crate::project::{
    Pattern, Project, BLOCK, COND_ALWAYS, COND_EVERY2, COND_EVERY4, COND_FILL, COND_NOTFILL, MAX_EVENTS,
    PADS, PATTERNS, SR,
};

// ============================================================================
//  跨线程共享：UI 只读这些原子量，绝不碰音频线程的内部状态
// ============================================================================
pub struct Shared {
    pub playing: AtomicBool,
    pub step_index: AtomicI32,
    pub meter_l: AtomicU32, // f32 bits
    pub meter_r: AtomicU32,
    pub cpu: AtomicU32,
    pub xruns: AtomicI32,
    pub clock_external: AtomicBool,
    pub audio_ok: AtomicBool,
    pub midi_on: AtomicBool,
    pub pattern: AtomicI32, // 供 MIDI/UI 显示当前 pattern
}

impl Shared {
    pub const fn new() -> Shared {
        Shared {
            playing: AtomicBool::new(false),
            step_index: AtomicI32::new(0),
            meter_l: AtomicU32::new(0),
            meter_r: AtomicU32::new(0),
            cpu: AtomicU32::new(0),
            xruns: AtomicI32::new(0),
            clock_external: AtomicBool::new(false),
            audio_ok: AtomicBool::new(false),
            midi_on: AtomicBool::new(false),
            pattern: AtomicI32::new(0),
        }
    }
    #[inline]
    pub fn meter(&self) -> (f32, f32) {
        (
            f32::from_bits(self.meter_l.load(Ordering::Relaxed)),
            f32::from_bits(self.meter_r.load(Ordering::Relaxed)),
        )
    }
    #[inline]
    pub fn cpu(&self) -> f32 {
        f32::from_bits(self.cpu.load(Ordering::Relaxed))
    }
}

/// 全局共享状态（全原子，静态初始化，线程安全）
pub static SHARED: Shared = Shared::new();

// ============================================================================
//  UI → 音频 的无锁单生产者/单消费者队列
//  一个 u64 打包一次“手点触发”：pad(4bit) | vel(7bit) | accent(1bit)
// ============================================================================
const QCAP: usize = 256;
static Q_BUF: [AtomicU64; QCAP] = [const { AtomicU64::new(0) }; QCAP];
static Q_HEAD: AtomicUsize = AtomicUsize::new(0);
static Q_TAIL: AtomicUsize = AtomicUsize::new(0);

/// UI / MIDI 线程调用：请求立刻（下个块内）敲一下某声部
pub fn ui_trigger_pad(pad: i32, vel: i32, accent: bool) {
    if !(0..PADS as i32).contains(&pad) {
        return;
    }
    let vel = vel.clamp(1, 127) as u64;
    let seat = Q_TAIL.load(Ordering::Acquire);
    let head = Q_HEAD.load(Ordering::Acquire);
    if seat.wrapping_sub(head) >= QCAP {
        return; // 队列满：直接丢，绝不阻塞 UI
    }
    let data = (pad as u64 & 0xf) | (vel << 4) | ((accent as u64) << 11);
    Q_BUF[seat % QCAP].store(data, Ordering::Release);
    Q_TAIL.store(seat.wrapping_add(1), Ordering::Release);
}

// ============================================================================
//  内部事件：绝对样本时刻 + 声部 + 力度 + 重音
// ============================================================================
#[derive(Clone, Copy)]
pub struct Event {
    pub t: i64,
    pub pad: i32,
    pub vel: f32,
    pub accent: bool,
}

// ============================================================================
//  引擎
// ============================================================================
pub struct Engine {
    pub proj: Project,
    pub voices: Vec<Vec<Voice>>,

    pub playing: bool,
    pub sample_pos: i64,   // 下一块的绝对起点
    pub next_step_abs: i64,
    pub step_index: i32,
    pub global_step: i64,
    pub cycle_count: i32,
    pub fill_now: bool,
    pub pending_pattern: i32,
    pub song_index: i32,
    pub song_repeat: i32,
    pub rng: u32,

    pending: Vec<Event>, // 音序器展开出来的待触发事件
    evbuf: Vec<Event>,   // 本块要消费的事件（已排序）
    scratch: Vec<Event>, // 复用缓冲，避免回调内分配

    pub l: [f32; BLOCK],
    pub r: [f32; BLOCK],

    hp_l: crate::dsp::Biquad,
    hp_r: crate::dsp::Biquad,
    lp_l: crate::dsp::Biquad,
    lp_r: crate::dsp::Biquad,

    pub clock_external: bool,
    pub clock_pulse: i32,
    pub solo_override: i32, // >=0 时（分轨导出）只让该轨出声

    // 参数一阶平滑状态（系数 0.002/样本），避免 zipper noise
    pad_level_s: [f32; PADS],
    pad_pan_s: [f32; PADS],
    vol_s: f32,
}

impl Engine {
    pub fn new(proj: Project) -> Engine {
        let voices = make_voices();
        let mut e = Engine {
            proj,
            voices,
            playing: false,
            sample_pos: 0,
            next_step_abs: 0,
            step_index: 0,
            global_step: 0,
            cycle_count: 0,
            fill_now: false,
            pending_pattern: -1,
            song_index: 0,
            song_repeat: 0,
            rng: 0x0080_8808,
            pending: Vec::with_capacity(MAX_EVENTS),
            evbuf: Vec::with_capacity(MAX_EVENTS),
            scratch: Vec::with_capacity(MAX_EVENTS),
            l: [0.0; BLOCK],
            r: [0.0; BLOCK],
            hp_l: crate::dsp::Biquad::new(),
            hp_r: crate::dsp::Biquad::new(),
            lp_l: crate::dsp::Biquad::new(),
            lp_r: crate::dsp::Biquad::new(),
            clock_external: false,
            clock_pulse: 0,
            solo_override: -1,
            pad_level_s: [0.0; PADS],
            pad_pan_s: [0.0; PADS],
            vol_s: 0.0,
        };
        e.rng = if e.proj.seed != 0 { e.proj.seed } else { 0x0080_8808 };
        // 噪声池：确定性伪随机，同一台机器每次启动音色都一致
        for i in 0..PADS {
            for v in 0..crate::project::MAX_VOICES {
                let mut st: u32 = 0x808u32.wrapping_add((i * 7919) as u32);
                for k in 0..8192 {
                    st = st.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                    let s = (st >> 8) as i32;
                    e.voices[i][v].noise[k] = (s as f32 / 8_388_608.0 - 1.0) as f32;
                }
            }
        }
        // 平滑器初值 = 当前参数，避免上电第一块从 0 爬升
        for i in 0..PADS {
            e.pad_level_s[i] = e.proj.pads[i].level;
            e.pad_pan_s[i] = e.proj.pads[i].pan;
        }
        e.vol_s = e.proj.master.volume;
        e
    }

    // ------------------------------ 传输 ------------------------------
    pub fn play(&mut self) {
        if self.playing {
            return;
        }
        self.playing = true;
        self.step_index = 0;
        self.global_step = 0;
        self.cycle_count = 0;
        self.fill_now = false;
        self.song_index = 0;
        self.song_repeat = 0;
        self.next_step_abs = self.sample_pos;
        self.pending.clear();
        SHARED.playing.store(true, Ordering::Relaxed);
        SHARED.step_index.store(0, Ordering::Relaxed);
    }

    pub fn stop(&mut self) {
        self.playing = false;
        // 10ms 淡出（480 样本），既马上安静又不会“咔”
        for p in 0..PADS {
            for v in 0..crate::project::MAX_VOICES {
                self.voices[p][v].kill(self.sample_pos, 480);
            }
        }
        self.pending.clear();
        SHARED.playing.store(false, Ordering::Relaxed);
    }

    /// 步长（样本）＝ 48000 * 60 / BPM / 4；BPM 变化不重置播放位置
    pub fn step_len(&self) -> i32 {
        let mut bpm = self.proj.master.bpm;
        if bpm < 40.0 {
            bpm = 40.0;
        }
        if bpm > 300.0 {
            bpm = 300.0;
        }
        let n = (SR as f64 * 60.0 / bpm as f64 / 4.0 + 0.5) as i32;
        n.max(64)
    }

    // ------------------------------ 音序 ------------------------------
    /// 展开一个 16 分步：把每轨每格的所有属性换算成绝对样本事件
    fn emit_step(&mut self, step_abs: i64) {
        let cur = self.proj.cur();
        let sl = self.step_len();
        let swing = self.proj.master.swing;
        let fill_now = self.fill_now;
        let cycle = self.cycle_count;
        let step_index = self.step_index;
        let global_step = self.global_step;

        // Swing：奇数 16 分步向后偏移 swing * stepSamples * 0.5，与 micro 叠加
        let swing_off = if step_index & 1 != 0 { (sl as f32 * swing * 0.5) as i64 } else { 0 };
        let base = step_abs + swing_off;

        let pat: &Pattern = &self.proj.patterns[cur];
        for pad in 0..PADS {
            // 每轨独立长度 → polymeter：轨道步号 = 全局步号 % trackLength
            let st = *pat.step(pad, global_step);
            if !st.on {
                continue;
            }
            // 条件触发
            let ok = match st.cond {
                COND_ALWAYS => true,
                COND_FILL => fill_now,
                COND_NOTFILL => !fill_now,
                COND_EVERY2 => cycle % 2 == 0,
                COND_EVERY4 => cycle % 4 == 0,
                _ => true,
            };
            if !ok {
                continue;
            }
            if st.prob == 0 {
                continue;
            }
            if st.prob < 100 {
                let mut x = self.rng;
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                self.rng = x;
                if (x % 100) as i32 >= st.prob as i32 {
                    continue;
                }
            }

            let mut vel = st.velocity as f32 / 127.0;
            if vel < 0.02 {
                vel = 0.02;
            }
            if vel > 1.0 {
                vel = 1.0;
            }
            if st.accent {
                vel *= 1.30;
            }
            if vel > 1.25 {
                vel = 1.25;
            }

            let r = i32::from(st.ratchet).clamp(1, 8);
            // Ratchet：把一步均分 N 个子事件，力度每个递减 8%
            for i in 0..r {
                let t = base + st.micro as i64 + (i as f64 * sl as f64 / r as f64) as i64;
                let vv = vel * 0.92f32.powi(i);
                self.pending.push(Event { t, pad: pad as i32, vel: vv, accent: st.accent });
            }
        }
    }

    /// 步进推进：切 pattern / 歌曲链只在循环边界发生
    fn advance_step(&mut self) {
        let cur = self.proj.cur();
        let steps = self.proj.patterns[cur].steps.max(1);
        self.step_index += 1;
        self.global_step += 1;
        if self.step_index < steps {
            return;
        }

        self.step_index = 0;
        self.cycle_count += 1;
        self.fill_now = (self.cycle_count % 4) == 3; // 每 4 个循环来一次过门

        // 播放中切 pattern：只在步边界生效
        if self.pending_pattern >= 0 {
            let n = self.pending_pattern.clamp(0, PATTERNS as i32 - 1);
            self.proj.current = n;
            self.pending_pattern = -1;
            self.song_repeat = 0;
            SHARED.pattern.store(n, Ordering::Relaxed);
            return;
        }
        // Song 模式：按 [{pattern, repeats}] 顺序播放，结束后循环
        if self.proj.master.song_mode && !self.proj.song.is_empty() {
            let count = self.proj.song.len() as i32;
            let idx = (self.song_index % count) as usize;
            let rep = self.proj.song[idx].repeats.max(1);
            self.song_repeat += 1;
            if self.song_repeat >= rep {
                self.song_repeat = 0;
                self.song_index = (self.song_index + 1) % count;
                let np = self.proj.song[(self.song_index % count) as usize].pattern.clamp(0, PATTERNS as i32 - 1);
                self.proj.current = np;
                SHARED.pattern.store(np, Ordering::Relaxed);
            }
        }
    }

    /// 声部是否可闻（mute/solo 在混音阶段阻断）
    fn audible(&self, pad: usize) -> bool {
        if self.solo_override >= 0 {
            return pad as i32 == self.solo_override;
        }
        let any_solo = self.proj.pads.iter().any(|p| p.solo);
        let p = &self.proj.pads[pad];
        if p.mute {
            return false;
        }
        if any_solo && !p.solo {
            return false;
        }
        true
    }

    /// 触发一个 voice，并处理 choke 与抢占
    fn trigger(&mut self, pad: i32, when: i64, vel: f32, accent: bool) {
        let pad = pad.clamp(0, PADS as i32 - 1) as usize;
        let mv = crate::project::MAX_VOICES;

        // CHOKE 组：闭镲(15) 掐开镲(14)；开镲重触发也先掐旧的
        if pad == 15 || pad == 14 {
            for i in 0..mv {
                self.voices[14][i].kill(when, 16);
            }
        }

        // 找空闲 voice；满了就抢占最老的
        let mut target: Option<usize> = None;
        for i in 0..mv {
            if !self.voices[pad][i].active {
                target = Some(i);
                break;
            }
        }
        let ti = match target {
            Some(i) => i,
            None => {
                let mut oldest = 0;
                for i in 1..mv {
                    if self.voices[pad][i].start < self.voices[pad][oldest].start {
                        oldest = i;
                    }
                }
                self.voices[pad][oldest].kill(when, 64);
                oldest
            }
        };
        self.voices[pad][ti].trigger(when, vel, accent);
    }

    // ------------------------------ 事件收集 ------------------------------
    /// 把本块要消费的事件收集到 evbuf，并按时间排序（插入排序，事件很少）
    fn collect_events(&mut self, b0: i64, b1: i64) {
        self.evbuf.clear();

        // 1) UI 手点事件：从无锁队列取走，落位到 b0+32（约 0.7ms 后发声，避免块边界爆点）
        let mut head = Q_HEAD.load(Ordering::Acquire);
        let tail = Q_TAIL.load(Ordering::Acquire);
        while head != tail {
            let d = Q_BUF[head % QCAP].load(Ordering::Acquire);
            head = head.wrapping_add(1);
            let pad = (d & 0xf) as i32;
            let vel = ((d >> 4) & 0x7f) as f32 / 127.0;
            let accent = ((d >> 11) & 1) != 0;
            self.pending.push(Event { t: self.sample_pos + 32, pad, vel, accent });
        }
        Q_HEAD.store(head, Ordering::Release);

        // 2) 音序器：本块内到点的步全部展开（与 C 版 guard 一致，最多 64 步防死循环）
        if self.playing && !self.clock_external {
            let mut guard = 0;
            while self.next_step_abs < b1 && guard < 64 {
                let at = if self.next_step_abs > b0 { self.next_step_abs } else { b0 };
                self.emit_step(at);
                let sl = self.step_len();
                self.next_step_abs += sl as i64;
                self.advance_step();
                guard += 1;
            }
        }

        // 3) 分离“本块”与“未来”的事件
        self.scratch.clear();
        for ev in self.pending.iter() {
            if ev.t < b1 {
                let mut e = *ev;
                if e.t < b0 {
                    e.t = b0;
                }
                self.evbuf.push(e);
            } else {
                self.scratch.push(*ev);
            }
        }
        std::mem::swap(&mut self.pending, &mut self.scratch);
        self.scratch.clear();

        // 4) 按时刻排序（同刻保持稳定顺序即可）
        let n = self.evbuf.len();
        for i in 1..n {
            let key = self.evbuf[i];
            let mut j = i;
            while j > 0 && self.evbuf[j - 1].t > key.t {
                self.evbuf[j] = self.evbuf[j - 1];
                j -= 1;
            }
            self.evbuf[j] = key;
        }
    }

    /// 渲染 [a,b) 区间内的所有 voice
    fn render_voices(&mut self, block0: i64, a: i64, b: i64) {
        if b <= a {
            return;
        }
        for pad in 0..PADS {
            if !self.audible(pad) {
                continue;
            }
            // 用平滑后的 level/pan（一阶平滑，避免滑杆拖动产生 zipper noise）
            let mut p = self.proj.pads[pad];
            p.level = self.pad_level_s[pad];
            p.pan = self.pad_pan_s[pad];
            for v in 0..crate::project::MAX_VOICES {
                if self.voices[pad][v].active {
                    render_voice(&mut self.voices[pad][v], &mut self.l, &mut self.r, block0, b, &p);
                }
            }
        }
    }

    /// 参数平滑：把一个块当作“按样本 0.002 系数”走完，等价于逐样本一阶低通
    fn update_smoothing(&mut self, n: usize) {
        let a = 1.0 - 0.998f32.powi(n as i32);
        for i in 0..PADS {
            let tl = self.proj.pads[i].level.clamp(0.0, 1.25);
            self.pad_level_s[i] += (tl - self.pad_level_s[i]) * a;
            let tp = self.proj.pads[i].pan.clamp(0.0, 1.0);
            self.pad_pan_s[i] += (tp - self.pad_pan_s[i]) * a;
        }
        let tv = self.proj.master.volume.clamp(0.0, 1.0);
        self.vol_s += (tv - self.vol_s) * a;
    }

    /// 主效果链（顺序固定）：HP → LP → 过载 tanh → 软限幅 → 主音量
    fn master_fx(&mut self, n: usize) {
        let m = self.proj.master;

        if m.hp_on {
            self.hp_l.set_hp(m.hp_cutoff as f64, m.hp_q as f64);
            self.hp_r.set_hp(m.hp_cutoff as f64, m.hp_q as f64);
            for i in 0..n {
                self.l[i] = self.hp_l.run(self.l[i] as f64) as f32;
                self.r[i] = self.hp_r.run(self.r[i] as f64) as f32;
            }
        }
        if m.lp_on {
            self.lp_l.set_lp(m.lp_cutoff as f64, m.lp_q as f64);
            self.lp_r.set_lp(m.lp_cutoff as f64, m.lp_q as f64);
            for i in 0..n {
                self.l[i] = self.lp_l.run(self.l[i] as f64) as f32;
                self.r[i] = self.lp_r.run(self.r[i] as f64) as f32;
            }
        }

        let drive = m.drive.clamp(1.0, 8.0);
        let norm = (drive as f64).tanh() as f32;
        let vol = self.vol_s;

        let mut pl = 0.0f32;
        let mut pr = 0.0f32;
        for i in 0..n {
            let mut xl = self.l[i];
            let mut xr = self.r[i];
            if drive > 1.001 {
                xl = ((xl * drive).tanh()) / norm;
                xr = ((xr * drive).tanh()) / norm;
            }
            // 软限幅：x/(1+|x|)，永不超过 ±1
            xl /= 1.0 + xl.abs();
            xr /= 1.0 + xr.abs();
            xl *= vol;
            xr *= vol;
            self.l[i] = xl;
            self.r[i] = xr;
            if xl.abs() > pl {
                pl = xl.abs();
            }
            if xr.abs() > pr {
                pr = xr.abs();
            }
        }
        // 电平表带 0.75 的峰值保持衰减，UI 看起来更稳
        let (ml, mr) = SHARED.meter();
        let nl = if pl > ml * 0.75 { pl } else { ml * 0.75 };
        let nr = if pr > mr * 0.75 { pr } else { mr * 0.75 };
        SHARED.meter_l.store(nl.to_bits(), Ordering::Relaxed);
        SHARED.meter_r.store(nr.to_bits(), Ordering::Relaxed);
    }

    /// 整个程序唯一的声音出口：渲染 n 个样本到 l/r
    pub fn render(&mut self, n: usize) {
        let n = n.min(BLOCK);
        let b0 = self.sample_pos;
        let b1 = b0 + n as i64;
        self.l[..n].fill(0.0);
        self.r[..n].fill(0.0);

        self.update_smoothing(n);
        self.collect_events(b0, b1);

        let cnt = self.evbuf.len();
        let mut cur = b0;
        for i in 0..cnt {
            let ev = self.evbuf[i];
            if ev.t > cur {
                self.render_voices(b0, cur, ev.t);
                cur = ev.t;
            }
            if ev.t < b1 {
                self.trigger(ev.pad, ev.t, ev.vel, ev.accent);
            }
        }
        if cur < b1 {
            self.render_voices(b0, cur, b1);
        }

        self.master_fx(n);

        self.sample_pos = b1;
        SHARED.step_index.store(self.step_index, Ordering::Relaxed);
        SHARED.clock_external.store(self.clock_external, Ordering::Relaxed);

        // 兜底回收：超过 4 秒的 voice 无条件释放（音频线程不做堆操作，只改标志）
        for p in 0..PADS {
            for v in 0..crate::project::MAX_VOICES {
                if self.voices[p][v].active && b0 - self.voices[p][v].start > 4 * SR as i64 {
                    self.voices[p][v].active = false;
                }
            }
        }
    }

    // ------------------------------ UI 接口 ------------------------------
    /// 播放中请求切 pattern：下一步边界生效
    pub fn request_pattern(&mut self, idx: i32) {
        let idx = idx.clamp(0, PATTERNS as i32 - 1);
        if self.playing {
            self.pending_pattern = idx;
        } else {
            self.proj.current = idx;
            SHARED.pattern.store(idx, Ordering::Relaxed);
        }
    }

    /// 外部 MIDI 时钟：每 6 个 0xF8（=96 PPQN / 6 = 16 分）推进一步
    pub fn external_tick(&mut self) {
        if !self.playing {
            return;
        }
        self.clock_pulse += 1;
        if self.clock_pulse < 6 {
            return;
        }
        self.clock_pulse = 0;
        let pos = self.sample_pos;
        self.emit_step(pos);
        self.advance_step();
        let sl = self.step_len();
        self.next_step_abs = pos + sl as i64;
    }
}
