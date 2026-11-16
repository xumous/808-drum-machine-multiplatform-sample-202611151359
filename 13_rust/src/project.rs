// ============================================================================
//  project.rs —— 工程数据模型 + 默认工程 + JSON 存档（作者：徐某）
//
//  这里的数据结构与 C/C#/Python 版一一对应，字段名也刻意保持一致，
//  因此同一个 .xu808（JSON）工程可以在各语言版本之间来回搬。
//
//  JSON 解析器完全手写（约 200 行）：支持 object / array / string /
//  number / true / false / null 六种值，够用且没有依赖。
//  加载策略：先用默认工程铺底，再用文件里的已知字段覆盖 —— 未知字段忽略、
//  缺失字段取默认值，这样旧文件在新版程序里也能打开。
// ============================================================================
use std::fs;
use std::io;

// ------------------------------ 全局常量 ------------------------------
pub const SR: usize = 48_000; // 采样率，所有版本统一 48kHz
pub const PADS: usize = 16; // 16 个声部（编号固定，不可重排）
pub const MAX_VOICES: usize = 4; // 每个声部最多 4 个并发 voice
pub const BLOCK: usize = 256; // 音频回调块长：256 样本 ≈ 5.33ms
pub const STEPS_MAX: usize = 64; // 单个 pattern 最大步数
pub const PATTERNS: usize = 16; // 工程里最多 16 个 pattern
pub const MAX_EVENTS: usize = 4096; // 事件队列容量

// 条件触发（数值即存储值）
pub const COND_ALWAYS: u8 = 0;
pub const COND_FILL: u8 = 1;
pub const COND_NOTFILL: u8 = 2;
pub const COND_EVERY2: u8 = 3;
pub const COND_EVERY4: u8 = 4;

/// 16 个声部的代号（顺序就是编号）
pub const CODES: [&str; PADS] = [
    "BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH",
];
/// 中文名（UI 上显示）
pub const NAMES_CN: [&str; PADS] = [
    "底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加", "边击", "击木", "拍手",
    "沙锤", "牛铃", "吊镲", "开镲", "闭镲",
];
/// MIDI 音高映射（导出与 MIDI 输入共用）
pub const MIDI_NOTES: [i32; PADS] = [
    36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42,
];

// ------------------------------ 数据结构 ------------------------------

/// 一个格子的全部属性。用 u8/i16 存储，16×64 网格只占约 8KB。
#[derive(Clone, Copy)]
pub struct Step {
    pub on: bool,
    pub velocity: u8, // 1..127
    pub accent: bool,
    pub prob: u8,   // 0..100
    pub micro: i16, // ±2400 样本
    pub ratchet: u8, // 1..8
    pub cond: u8,   // 0..4
}

impl Default for Step {
    fn default() -> Self {
        // 默认力度 100、概率 100、无滚奏 —— 和 C 版 memset 后统一赋的值一致
        Step { on: false, velocity: 100, accent: false, prob: 100, micro: 0, ratchet: 1, cond: 0 }
    }
}

/// 一个 Pattern：16 轨 × 64 步 + 每轨独立长度（polymeter）
#[derive(Clone)]
pub struct Pattern {
    pub name: String,
    pub steps: i32,
    pub fill: bool,
    pub data: Vec<Vec<Step>>, // [pad][step]
    pub track_len: [u8; PADS],
}

impl Pattern {
    pub fn new(idx: usize) -> Pattern {
        Pattern {
            name: format!("PATTERN {}", idx + 1),
            steps: 16,
            fill: false,
            data: vec![vec![Step::default(); STEPS_MAX]; PADS],
            track_len: [16u8; PADS],
        }
    }

    /// 取得某轨某全局步对应的格子（自动按 trackLen 取模）
    #[inline]
    pub fn step(&self, pad: usize, global_step: i64) -> &Step {
        let mut tl = self.track_len[pad] as i64;
        if tl < 1 {
            tl = 1;
        }
        if tl > STEPS_MAX as i64 {
            tl = STEPS_MAX as i64;
        }
        let idx = (global_step % tl) as usize;
        &self.data[pad][idx]
    }
}

/// 每声部参数：0..1（level 0..1.25），映射到 DSP 物理量
#[derive(Clone, Copy)]
pub struct PadParams {
    pub tune: f32,
    pub decay: f32,
    pub tone: f32,
    pub snappy: f32,
    pub tail: f32,
    pub level: f32,
    pub pan: f32,
    pub mute: bool,
    pub solo: bool,
}

impl Default for PadParams {
    fn default() -> Self {
        PadParams {
            tune: 0.50,
            decay: 0.50,
            tone: 0.50,
            snappy: 0.55,
            tail: 0.45,
            level: 0.75,
            pan: 0.50,
            mute: false,
            solo: false,
        }
    }
}

/// 主效果与全局设置
#[derive(Clone, Copy)]
pub struct Master {
    pub volume: f32,
    pub drive: f32,
    pub lp_cutoff: f32,
    pub lp_q: f32,
    pub hp_cutoff: f32,
    pub hp_q: f32,
    pub lp_on: bool,
    pub hp_on: bool,
    pub bpm: f32,
    pub swing: f32,
    pub beats_per_bar: i32,
    pub beat_unit: i32,
    pub song_mode: bool,
}

/// 歌曲链的一节
#[derive(Clone, Copy)]
pub struct SongEntry {
    pub pattern: i32,
    pub repeats: i32,
}

/// 顶层工程
#[derive(Clone)]
pub struct Project {
    pub name: String,
    pub patterns: Vec<Pattern>,
    pub pads: Vec<PadParams>,
    pub master: Master,
    pub song: Vec<SongEntry>,
    pub current: i32,
    pub seed: u32,
}

impl Project {
    /// 默认工程：完全照抄 C 版 xu_project_default 的数值
    pub fn default_project() -> Project {
        let mut pads = vec![PadParams::default(); PADS];
        pads[0].decay = 0.55; // 底鼓稍长
        pads[0].tone = 0.45;
        pads[1].decay = 0.45;

        let mut patterns: Vec<Pattern> = (0..PATTERNS).map(Pattern::new).collect();
        demo(&mut patterns[0]);

        Project {
            name: "XuMou Xu808 Project".to_string(),
            patterns,
            pads,
            master: Master {
                volume: 0.80,
                drive: 1.35,
                lp_cutoff: 18000.0,
                lp_q: 0.90,
                hp_cutoff: 28.0,
                hp_q: 0.70,
                lp_on: true,
                hp_on: true,
                bpm: 120.0,
                swing: 0.0,
                beats_per_bar: 4,
                beat_unit: 4,
                song_mode: false,
            },
            song: vec![SongEntry { pattern: 0, repeats: 4 }, SongEntry { pattern: 0, repeats: 4 }],
            current: 0,
            seed: 0x0080_8808,
        }
    }

    /// 当前 pattern 的安全索引（永远落在 0..PATTERNS 内）
    #[inline]
    pub fn cur(&self) -> usize {
        let c = self.current;
        if c < 0 {
            0
        } else if c as usize >= PATTERNS {
            PATTERNS - 1
        } else {
            c as usize
        }
    }
}

/// 演示律动：和 C 版逐格一致（底鼓、军鼓、闭镲、开镲、拍手、沙锤、滚奏）
pub fn demo(pat: &mut Pattern) {
    pat.steps = 16;
    pat.name = "PATTERN 1".to_string();
    for p in 0..PADS {
        for s in 0..STEPS_MAX {
            pat.data[p][s] = Step::default();
        }
    }

    // 底鼓：正拍 + 反拍 + 第 14 步补一下
    pat.data[0][0] = Step { on: true, velocity: 118, accent: true, ..Step::default() };
    pat.data[0][8] = Step { on: true, velocity: 105, ..Step::default() };
    pat.data[0][14] = Step { on: true, velocity: 90, ..Step::default() };
    // 军鼓：2、4 拍
    pat.data[1][4] = Step { on: true, velocity: 110, ..Step::default() };
    pat.data[1][12] = Step { on: true, velocity: 110, accent: true, ..Step::default() };
    // 闭镲：八分音符，正拍稍重
    let mut s = 0;
    while s < 16 {
        pat.data[15][s] = Step { on: true, velocity: if s % 4 == 0 { 96 } else { 84 }, ..Step::default() };
        s += 2;
    }
    // 开镲 / 拍手 / 沙锤
    pat.data[14][14] = Step { on: true, velocity: 92, ..Step::default() };
    pat.data[10][12] = Step { on: true, velocity: 80, ..Step::default() };
    let mut s = 1;
    while s < 16 {
        pat.data[11][s] = Step { on: true, velocity: 42, ..Step::default() };
        s += 2;
    }
    // 第 15 步底鼓三连滚奏，直接展示 ratchet
    pat.data[0][15] = Step { on: true, velocity: 88, ratchet: 3, ..Step::default() };
}

// ============================================================================
//  JSON 存档
// ============================================================================

fn esc(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o
}

impl Project {
    /// 保存为 JSON（手写序列化，字段名与规格书第 7 节一致）
    pub fn save_json(&self, path: &str) -> io::Result<()> {
        let mut s = String::with_capacity(64 * 1024);
        s.push_str("{\n");
        s.push_str("\"format\":\"XU808\",\"version\":1,\n");
        s.push_str(&format!("\"name\":\"{}\",\"current\":{},\"seed\":{},\n", esc(&self.name), self.current, self.seed));

        let m = &self.master;
        s.push_str("\"master\":{");
        s.push_str(&format!(
            "\"volume\":{:.6},\"drive\":{:.6},\"lpCutoff\":{:.2},\"lpQ\":{:.4},\"hpCutoff\":{:.2},\"hpQ\":{:.4},",
            m.volume, m.drive, m.lp_cutoff, m.lp_q, m.hp_cutoff, m.hp_q
        ));
        s.push_str(&format!(
            "\"lpOn\":{},\"hpOn\":{},\"bpm\":{:.3},\"swing\":{:.4},\"beatsPerBar\":{},\"beatUnit\":{},\"songMode\":{}}},\n",
            m.lp_on as i32, m.hp_on as i32, m.bpm, m.swing, m.beats_per_bar, m.beat_unit, m.song_mode as i32
        ));

        s.push_str("\"pads\":[\n");
        for (i, p) in self.pads.iter().enumerate() {
            s.push_str(&format!(
                "{{\"tune\":{:.4},\"decay\":{:.4},\"tone\":{:.4},\"snappy\":{:.4},\"tail\":{:.4},\"level\":{:.4},\"pan\":{:.4},\"mute\":{},\"solo\":{}}}",
                p.tune, p.decay, p.tone, p.snappy, p.tail, p.level, p.pan, p.mute as i32, p.solo as i32
            ));
            if i + 1 < self.pads.len() {
                s.push(',');
            }
            s.push('\n');
        }
        s.push_str("],\n");

        s.push_str("\"patterns\":[\n");
        for (pi, pat) in self.patterns.iter().enumerate() {
            s.push_str(&format!("{{\"name\":\"{}\",\"steps\":{},\"fill\":{},\"trackLen\":[", esc(&pat.name), pat.steps, pat.fill as i32));
            for t in 0..PADS {
                if t > 0 {
                    s.push(',');
                }
                s.push_str(&pat.track_len[t].to_string());
            }
            s.push_str("],\"data\":[");
            for pad in 0..PADS {
                if pad > 0 {
                    s.push(',');
                }
                s.push('[');
                for st in 0..STEPS_MAX {
                    let x = &pat.data[pad][st];
                    if st > 0 {
                        s.push(',');
                    }
                    s.push_str(&format!(
                        "[{},{},{},{},{},{},{}]",
                        x.on as i32, x.velocity, x.accent as i32, x.prob, x.micro, x.ratchet, x.cond
                    ));
                }
                s.push(']');
            }
            s.push_str("]}");
            if pi + 1 < self.patterns.len() {
                s.push(',');
            }
            s.push('\n');
        }
        s.push_str("],\n");

        s.push_str("\"song\":[");
        for (i, e) in self.song.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            s.push_str(&format!("{{\"pattern\":{},\"repeats\":{}}}", e.pattern, e.repeats));
        }
        s.push_str("]\n}\n");

        fs::write(path, s)
    }

    /// 加载 JSON：默认值铺底 + 已知字段覆盖 + 范围钳制
    pub fn load_json(path: &str) -> io::Result<Project> {
        let text = fs::read_to_string(path)?;
        let mut p = Project::default_project();
        let v = match parse_json(&text) {
            Some(v) => v,
            None => return Ok(p), // 解析失败就当默认工程，不炸掉 UI
        };

        if let Some(s) = v.get("name").and_then(J::as_str) {
            p.name = s.to_string();
        }
        if let Some(n) = v.get("current").and_then(J::as_i32) {
            p.current = n.clamp(0, PATTERNS as i32 - 1);
        }
        if let Some(n) = v.get("seed").and_then(J::as_i32) {
            p.seed = n as u32;
        }

        if let Some(m) = v.get("master") {
            let t = &mut p.master;
            if let Some(x) = m.get("volume").and_then(J::as_f32) {
                t.volume = x.clamp(0.0, 1.0);
            }
            if let Some(x) = m.get("drive").and_then(J::as_f32) {
                t.drive = x.clamp(1.0, 8.0);
            }
            if let Some(x) = m.get("lpCutoff").and_then(J::as_f32) {
                t.lp_cutoff = x.clamp(200.0, 20000.0);
            }
            if let Some(x) = m.get("lpQ").and_then(J::as_f32) {
                t.lp_q = x.clamp(0.5, 8.0);
            }
            if let Some(x) = m.get("hpCutoff").and_then(J::as_f32) {
                t.hp_cutoff = x.clamp(10.0, 800.0);
            }
            if let Some(x) = m.get("hpQ").and_then(J::as_f32) {
                t.hp_q = x.clamp(0.1, 8.0);
            }
            if let Some(x) = m.get("lpOn").and_then(J::as_bool) {
                t.lp_on = x;
            }
            if let Some(x) = m.get("hpOn").and_then(J::as_bool) {
                t.hp_on = x;
            }
            if let Some(x) = m.get("bpm").and_then(J::as_f32) {
                t.bpm = x.clamp(40.0, 300.0);
            }
            if let Some(x) = m.get("swing").and_then(J::as_f32) {
                t.swing = x.clamp(0.0, 0.75);
            }
            if let Some(x) = m.get("beatsPerBar").and_then(J::as_i32) {
                t.beats_per_bar = x.clamp(1, 16);
            }
            if let Some(x) = m.get("beatUnit").and_then(J::as_i32) {
                t.beat_unit = if x == 8 { 8 } else { 4 };
            }
            if let Some(x) = m.get("songMode").and_then(J::as_bool) {
                t.song_mode = x;
            }
        }

        if let Some(J::Arr(arr)) = v.get("pads") {
            for (i, pv) in arr.iter().enumerate() {
                if i >= PADS {
                    break;
                }
                let d = &mut p.pads[i];
                if let Some(x) = pv.get("tune").and_then(J::as_f32) {
                    d.tune = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("decay").and_then(J::as_f32) {
                    d.decay = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("tone").and_then(J::as_f32) {
                    d.tone = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("snappy").and_then(J::as_f32) {
                    d.snappy = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("tail").and_then(J::as_f32) {
                    d.tail = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("level").and_then(J::as_f32) {
                    d.level = x.clamp(0.0, 1.25);
                }
                if let Some(x) = pv.get("pan").and_then(J::as_f32) {
                    d.pan = x.clamp(0.0, 1.0);
                }
                if let Some(x) = pv.get("mute").and_then(J::as_bool) {
                    d.mute = x;
                }
                if let Some(x) = pv.get("solo").and_then(J::as_bool) {
                    d.solo = x;
                }
            }
        }

        if let Some(J::Arr(arr)) = v.get("patterns") {
            for (i, pv) in arr.iter().enumerate() {
                if i >= PATTERNS {
                    break;
                }
                let pat = &mut p.patterns[i];
                if let Some(s) = pv.get("name").and_then(J::as_str) {
                    pat.name = s.to_string();
                }
                if let Some(x) = pv.get("steps").and_then(J::as_i32) {
                    pat.steps = x.clamp(1, STEPS_MAX as i32);
                }
                if let Some(x) = pv.get("fill").and_then(J::as_bool) {
                    pat.fill = x;
                }
                if let Some(J::Arr(tl)) = pv.get("trackLen") {
                    for t in 0..PADS {
                        if let Some(x) = tl.get(t).and_then(J::as_i32) {
                            pat.track_len[t] = x.clamp(1, STEPS_MAX as i32) as u8;
                        }
                    }
                }
                if let Some(J::Arr(rows)) = pv.get("data") {
                    for (pad, row) in rows.iter().enumerate() {
                        if pad >= PADS {
                            break;
                        }
                        if let J::Arr(cells) = row {
                            for (st, cell) in cells.iter().enumerate() {
                                if st >= STEPS_MAX {
                                    break;
                                }
                                if let J::Arr(f) = cell {
                                    let g = |k: usize, def: f64| -> f64 {
                                        f.get(k).and_then(J::as_f64).unwrap_or(def)
                                    };
                                    let s = &mut pat.data[pad][st];
                                    s.on = g(0, 0.0) != 0.0;
                                    s.velocity = g(1, 100.0).clamp(1.0, 127.0) as u8;
                                    s.accent = g(2, 0.0) != 0.0;
                                    s.prob = g(3, 100.0).clamp(0.0, 100.0) as u8;
                                    s.micro = g(4, 0.0).clamp(-2400.0, 2400.0) as i16;
                                    s.ratchet = g(5, 1.0).clamp(1.0, 8.0) as u8;
                                    s.cond = g(6, 0.0).clamp(0.0, 4.0) as u8;
                                }
                            }
                        }
                    }
                }
            }
        }

        if let Some(J::Arr(arr)) = v.get("song") {
            p.song.clear();
            for sv in arr.iter() {
                let pat = sv.get("pattern").and_then(J::as_i32).unwrap_or(0).clamp(0, PATTERNS as i32 - 1);
                let rep = sv.get("repeats").and_then(J::as_i32).unwrap_or(1).max(1);
                p.song.push(SongEntry { pattern: pat, repeats: rep });
                if p.song.len() >= 64 {
                    break;
                }
            }
        }
        Ok(p)
    }
}

// ------------------------------ 极简 JSON ------------------------------

#[derive(Debug)]
pub enum J {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<J>),
    Obj(Vec<(String, J)>),
}

impl J {
    pub fn get(&self, key: &str) -> Option<&J> {
        if let J::Obj(m) = self {
            m.iter().find(|(k, _)| k == key).map(|(_, v)| v)
        } else {
            None
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            J::Num(n) => Some(*n),
            J::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
            _ => None,
        }
    }
    pub fn as_i32(&self) -> Option<i32> {
        self.as_f64().map(|v| v as i32)
    }
    pub fn as_f32(&self) -> Option<f32> {
        self.as_f64().map(|v| v as f32)
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            J::Bool(b) => Some(*b),
            J::Num(n) => Some(*n != 0.0),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            J::Str(s) => Some(s),
            _ => None,
        }
    }
}

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}

impl<'a> Parser<'a> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }
    fn peek(&self) -> u8 {
        if self.i < self.b.len() {
            self.b[self.i]
        } else {
            0
        }
    }
    fn value(&mut self) -> Option<J> {
        self.ws();
        match self.peek() {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => self.string().map(J::Str),
            b't' => self.lit("true", J::Bool(true)),
            b'f' => self.lit("false", J::Bool(false)),
            b'n' => self.lit("null", J::Null),
            b'-' | b'0'..=b'9' => self.number(),
            _ => None,
        }
    }
    fn lit(&mut self, word: &str, v: J) -> Option<J> {
        if self.b.len() >= self.i + word.len() && &self.b[self.i..self.i + word.len()] == word.as_bytes() {
            self.i += word.len();
            Some(v)
        } else {
            None
        }
    }
    fn number(&mut self) -> Option<J> {
        let start = self.i;
        if self.peek() == b'-' {
            self.i += 1;
        }
        while matches!(self.peek(), b'0'..=b'9' | b'.' | b'e' | b'E' | b'+' | b'-') {
            self.i += 1;
        }
        let s = std::str::from_utf8(&self.b[start..self.i]).ok()?;
        s.parse::<f64>().ok().map(J::Num)
    }
    fn string(&mut self) -> Option<String> {
        if self.peek() != b'"' {
            return None;
        }
        self.i += 1;
        let mut out = String::new();
        loop {
            let c = self.peek();
            if c == 0 {
                return None;
            }
            self.i += 1;
            match c {
                b'"' => return Some(out),
                b'\\' => {
                    let e = self.peek();
                    self.i += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            // \uXXXX：手工拼一个 char（代理对按单个码位处理）
                            let mut cp: u32 = 0;
                            for _ in 0..4 {
                                let h = self.peek();
                                self.i += 1;
                                let d = match h {
                                    b'0'..=b'9' => h - b'0',
                                    b'a'..=b'f' => h - b'a' + 10,
                                    b'A'..=b'F' => h - b'A' + 10,
                                    _ => return None,
                                };
                                cp = cp * 16 + d as u32;
                            }
                            out.push(char::from_u32(cp).unwrap_or('?'));
                        }
                        _ => return None,
                    }
                }
                _ => {
                    // UTF-8 多字节：把整个字符切片交给 from_utf8
                    let start = self.i - 1;
                    let len = if c < 0x80 {
                        1
                    } else if c >> 5 == 0b110 {
                        2
                    } else if c >> 4 == 0b1110 {
                        3
                    } else {
                        4
                    };
                    self.i = start + len;
                    if self.i > self.b.len() {
                        return None;
                    }
                    out.push_str(std::str::from_utf8(&self.b[start..self.i]).ok()?);
                }
            }
        }
    }
    fn array(&mut self) -> Option<J> {
        self.i += 1; // [
        let mut v = Vec::new();
        loop {
            self.ws();
            if self.peek() == b']' {
                self.i += 1;
                return Some(J::Arr(v));
            }
            v.push(self.value()?);
            self.ws();
            if self.peek() == b',' {
                self.i += 1;
            } else if self.peek() == b']' {
                self.i += 1;
                return Some(J::Arr(v));
            } else {
                return None;
            }
        }
    }
    fn object(&mut self) -> Option<J> {
        self.i += 1; // {
        let mut v = Vec::new();
        loop {
            self.ws();
            if self.peek() == b'}' {
                self.i += 1;
                return Some(J::Obj(v));
            }
            let k = self.string()?;
            self.ws();
            if self.peek() != b':' {
                return None;
            }
            self.i += 1;
            let val = self.value()?;
            v.push((k, val));
            self.ws();
            if self.peek() == b',' {
                self.i += 1;
            } else if self.peek() == b'}' {
                self.i += 1;
                return Some(J::Obj(v));
            } else {
                return None;
            }
        }
    }
}

pub fn parse_json(text: &str) -> Option<J> {
    let mut p = Parser { b: text.as_bytes(), i: 0 };
    p.value()
}
