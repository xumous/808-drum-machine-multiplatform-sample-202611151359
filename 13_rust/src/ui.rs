// ============================================================================
//  ui.rs —— Win32 原生界面（作者：徐某）
//
//  没有 Qt、没有 egui、没有任何 UI 框架，全部用 GDI 手绘：
//    · 顶部走带条：LOGO / Play / Stop / BPM / Tap / Swing / Pattern / Steps /
//      Song / MIDI CLK / 保存 / 打开 / 导出 WAV / 导出 MIDI；
//    · 中部 16×N（1..64 步）网格：左键开关、右键选中并编辑属性；
//    · 网格下方是“步进属性”条：ON/ACC/VEL/PROB/MIC/RAT/COND；
//    · 再下面是当前声部参数条 + 主效果（滤波/Drive/音量/电平表）。
//
//  UI 每 33ms 重绘一次（30fps），只读原子快照，绝不阻塞音频线程。
// ============================================================================
use crate::audio;
use crate::engine::{ui_trigger_pad, Engine, SHARED};
use crate::export;
use crate::midi;
use crate::project::{PadParams, Project, PATTERNS, STEPS_MAX};
use crate::win::*;

// ------------------------------ 配色（与其它版本一致） ------------------------------
const fn rgb(r: u32, g: u32, b: u32) -> COLORREF {
    r | (g << 8) | (b << 16)
}
const C_BG: COLORREF = rgb(18, 18, 18);
const C_PANEL: COLORREF = rgb(28, 28, 28);
const C_PANEL2: COLORREF = rgb(35, 35, 35);
const C_LINE: COLORREF = rgb(46, 46, 46);
const C_ORANGE: COLORREF = rgb(255, 122, 24);
const C_ORANGED: COLORREF = rgb(168, 74, 12);
const C_YELLOW: COLORREF = rgb(255, 209, 102);
const C_CYAN: COLORREF = rgb(76, 201, 240);
const C_TEXT: COLORREF = rgb(232, 232, 232);
const C_DIM: COLORREF = rgb(138, 138, 138);
const C_RED: COLORREF = rgb(229, 72, 77);
const C_GREEN: COLORREF = rgb(61, 220, 132);
const C_CELL_OFF: COLORREF = rgb(16, 16, 16);
const C_CELL_BORDER: COLORREF = rgb(39, 39, 39);
const C_ROW_A: COLORREF = rgb(22, 22, 22);
const C_ROW_B: COLORREF = rgb(26, 26, 26);
const C_BEAT_LINE: COLORREF = rgb(58, 58, 58);

// ------------------------------ 布局 ------------------------------
const TOP_H: i32 = 64;
const GX: i32 = 8;
const GY: i32 = 72;
// 通道条要容纳：代号 / 中文名 / M / S / 每轨长度（polymeter）三个控件
const LABEL_W: i32 = 172;
const HEAD_H: i32 = 22;
const CELL_H: i32 = 22;
const GRID_ROWS: i32 = 16;
const STEP_ED_Y: i32 = GY + HEAD_H + GRID_ROWS * CELL_H + 8;
const STEP_ED_H: i32 = 64;
const PARAM_Y: i32 = STEP_ED_Y + STEP_ED_H + 8;
const PARAM_H: i32 = 100;
const MASTER_Y: i32 = PARAM_Y + PARAM_H + 8;
const MASTER_H: i32 = 100;

// 歌曲链编辑弹窗（非模态，独立窗口类）
const SG_W: i32 = 620;
const SG_H: i32 = 420;
const SG_LIST_Y: i32 = 46;
const SG_ROW_H: i32 = 28;
const SG_ROWS: usize = 10;
const SG_BTNS: usize = 7; // 每行控件：P- / P+ / R- / R+ / 上移 / 下移 / 删除

const ZRECT: RECT = RECT { left: 0, top: 0, right: 0, bottom: 0 };

// ------------------------------ 全局 UI 状态 ------------------------------
static mut G_ENG: *mut Engine = std::ptr::null_mut();
static mut G_HWND: HWND = std::ptr::null_mut();
static mut G_SEL_PAD: i32 = 0;
static mut G_SEL_STEP: i32 = 0;
static mut G_DRAG_PARAM: i32 = -1;
static mut G_DRAG_MASTER: i32 = -1;
static mut G_CELL_W: i32 = 30;
static mut G_STEPS: i32 = 16;

// 歌曲链编辑窗口的运行期状态（选中项 / 列表滚动 / 控件命中区）
static mut G_SONG_HWND: HWND = std::ptr::null_mut();
static mut G_SONG_SEL: usize = 0;
static mut G_SONG_TOP: usize = 0;
static mut G_SONG_RC: [[RECT; SG_BTNS]; SG_ROWS] = [[ZRECT; SG_BTNS]; SG_ROWS];
static mut G_SONG_ADD: RECT = ZRECT;
static mut G_SONG_CLR: RECT = ZRECT;
static mut G_SONG_CLOSE: RECT = ZRECT;
static mut G_SONG_UP: RECT = ZRECT;
static mut G_SONG_DN: RECT = ZRECT;

static mut G_FONT_SMALL: HFONT = std::ptr::null_mut();
static mut G_FONT_NORM: HFONT = std::ptr::null_mut();
static mut G_FONT_BOLD: HFONT = std::ptr::null_mut();
static mut G_FONT_MONO: HFONT = std::ptr::null_mut();
static mut G_FONT_LOGO: HFONT = std::ptr::null_mut();
static mut G_FONT_TINY: HFONT = std::ptr::null_mut();

static mut G_RC_PLAY: RECT = ZRECT;
static mut G_RC_STOP: RECT = ZRECT;
static mut G_RC_TAP: RECT = ZRECT;
static mut G_RC_BPM_M: RECT = ZRECT;
static mut G_RC_BPM_V: RECT = ZRECT;
static mut G_RC_BPM_P: RECT = ZRECT;
static mut G_RC_SW_M: RECT = ZRECT;
static mut G_RC_SW_V: RECT = ZRECT;
static mut G_RC_SW_P: RECT = ZRECT;
static mut G_RC_PAT_M: RECT = ZRECT;
static mut G_RC_PAT_V: RECT = ZRECT;
static mut G_RC_PAT_P: RECT = ZRECT;
static mut G_RC_ST_M: RECT = ZRECT;
static mut G_RC_ST_V: RECT = ZRECT;
static mut G_RC_ST_P: RECT = ZRECT;
static mut G_RC_SONG: RECT = ZRECT;
static mut G_RC_SONGED: RECT = ZRECT; // 歌曲链编辑按钮
static mut G_RC_CLOCK: RECT = ZRECT;
static mut G_RC_SAVE: RECT = ZRECT;
static mut G_RC_LOAD: RECT = ZRECT;
static mut G_RC_WAV: RECT = ZRECT;
static mut G_RC_MIDI: RECT = ZRECT;
static mut G_RC_SIG: RECT = ZRECT; // 拍号小标签（MASTER 面板左上）
static mut G_RC_LP_ON: RECT = ZRECT; // 主效果低通开关
static mut G_RC_HP_ON: RECT = ZRECT; // 主效果高通开关
static mut G_RC_STEPED: [[RECT; 3]; 7] = [[ZRECT; 3]; 7];

/// 画笔/刷子缓存：一帧要画上千个矩形，反复 Create/Delete 会拖慢 UI
static mut BRUSH_CACHE: [(COLORREF, HBRUSH); 24] = [(0, std::ptr::null_mut()); 24];
static mut PEN_CACHE: [(COLORREF, HPEN); 12] = [(0, std::ptr::null_mut()); 12];

#[inline]
unsafe fn eng() -> &'static mut Engine {
    &mut *G_ENG
}

// ============================================================================
//  GDI 小工具
// ============================================================================
unsafe fn brush(c: COLORREF) -> HBRUSH {
    for slot in BRUSH_CACHE.iter() {
        if slot.1 != std::ptr::null_mut() && slot.0 == c {
            return slot.1;
        }
    }
    for i in 0..BRUSH_CACHE.len() {
        if BRUSH_CACHE[i].1 == std::ptr::null_mut() {
            let b = CreateSolidBrush(c);
            BRUSH_CACHE[i] = (c, b);
            return b;
        }
    }
    CreateSolidBrush(c)
}

unsafe fn pen(c: COLORREF) -> HPEN {
    for slot in PEN_CACHE.iter() {
        if slot.1 != std::ptr::null_mut() && slot.0 == c {
            return slot.1;
        }
    }
    for i in 0..PEN_CACHE.len() {
        if PEN_CACHE[i].1 == std::ptr::null_mut() {
            let p = CreatePen(PS_SOLID, 1, c);
            PEN_CACHE[i] = (c, p);
            return p;
        }
    }
    CreatePen(PS_SOLID, 1, c)
}

unsafe fn fill(dc: HDC, r: RECT, c: COLORREF) {
    if r.w() <= 0 || r.h() <= 0 {
        return;
    }
    let b = brush(c);
    FillRect(dc, &r, b);
}

unsafe fn frame(dc: HDC, r: RECT, c: COLORREF) {
    if r.w() <= 0 || r.h() <= 0 {
        return;
    }
    let p = pen(c);
    let old_pen = SelectObject(dc, p);
    let old_brush = SelectObject(dc, stock_object(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

/// GetStockObject 只在 gdi32 里声明，这里加个薄封装，避免多处写 unsafe
unsafe fn stock_object(i: i32) -> HGDIOBJ {
    get_stock_object(i)
}

#[link(name = "gdi32")]
extern "system" {
    #[link_name = "GetStockObject"]
    fn get_stock_object(i: i32) -> HGDIOBJ;
}

unsafe fn text_at(dc: HDC, x: i32, y: i32, w: i32, h: i32, s: &str, c: COLORREF, f: HFONT, fmt: UINT) {
    let ws = wide(s);
    let mut r = RECT::new(x, y, x + w, y + h);
    SelectObject(dc, f as HGDIOBJ);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, ws.as_ptr(), -1, &mut r, fmt | DT_SINGLELINE | DT_VCENTER);
}

unsafe fn draw_btn(dc: HDC, r: RECT, label: &str, active: bool) {
    fill(dc, r, if active { C_ORANGED } else { C_PANEL2 });
    frame(dc, r, if active { C_ORANGE } else { C_LINE });
    text_at(dc, r.left, r.top, r.w(), r.h(), label, C_TEXT, G_FONT_BOLD, DT_CENTER);
}

/// 水平滑杆：返回轨道矩形（供命中测试复核）
unsafe fn draw_slider(dc: HDC, x: i32, y: i32, w: i32, label: &str, v01: f32) -> RECT {
    text_at(dc, x, y, w, 14, label, C_DIM, G_FONT_TINY, DT_LEFT);
    let track = RECT::new(x, y + 20, x + w, y + 28);
    fill(dc, track, rgb(16, 16, 16));
    let mut hx = x + (v01 * (w - 12) as f32) as i32;
    if hx < x {
        hx = x;
    }
    if hx > x + w - 12 {
        hx = x + w - 12;
    }
    let handle = RECT::new(hx, y + 16, hx + 12, y + 32);
    fill(dc, handle, C_ORANGE);
    frame(dc, handle, C_LINE);
    track
}

unsafe fn slider_value(track: RECT, mx: i32) -> f32 {
    let w = track.w();
    if w <= 0 {
        return 0.0;
    }
    let v = (mx - track.left) as f32 / w as f32;
    v.clamp(0.0, 1.0)
}

// ============================================================================
//  声部参数定义（每种声部显示不同参数，与规格书第 3 节一致）
// ============================================================================
#[derive(Clone, Copy, PartialEq)]
enum PId {
    Tune,
    Decay,
    Tone,
    Snappy,
    Tail,
    Level,
    Pan,
}

fn pget(p: &PadParams, id: PId) -> f32 {
    match id {
        PId::Tune => p.tune,
        PId::Decay => p.decay,
        PId::Tone => p.tone,
        PId::Snappy => p.snappy,
        PId::Tail => p.tail,
        PId::Level => p.level,
        PId::Pan => p.pan,
    }
}

fn pset(p: &mut PadParams, id: PId, v: f32) {
    match id {
        PId::Tune => p.tune = v,
        PId::Decay => p.decay = v,
        PId::Tone => p.tone = v,
        PId::Snappy => p.snappy = v,
        PId::Tail => p.tail = v,
        PId::Level => p.level = v,
        PId::Pan => p.pan = v,
    }
}

fn param_defs(pad: i32) -> Vec<(PId, &'static str, f32)> {
    let mut v: Vec<(PId, &'static str, f32)> = Vec::with_capacity(6);
    match pad {
        0 => {
            v.push((PId::Tune, "TUNE", 100.0));
            v.push((PId::Decay, "DECAY", 100.0));
            v.push((PId::Tone, "TONE", 100.0));
        }
        1 => {
            v.push((PId::Tune, "TUNE", 100.0));
            v.push((PId::Snappy, "SNAP", 100.0));
            v.push((PId::Decay, "DECAY", 100.0));
        }
        2..=4 => {
            v.push((PId::Tune, "TUNE", 100.0));
            v.push((PId::Decay, "DECAY", 100.0));
            v.push((PId::Tone, "COLOR", 100.0));
        }
        10 => {
            v.push((PId::Decay, "DECAY", 100.0));
            v.push((PId::Tone, "TONE", 100.0));
            v.push((PId::Tail, "TAIL", 100.0));
        }
        11 => {
            v.push((PId::Decay, "DECAY", 100.0));
            v.push((PId::Tone, "TONE", 100.0));
        }
        12 => {
            v.push((PId::Tune, "TUNE", 100.0));
            v.push((PId::Decay, "DECAY", 100.0));
        }
        13..=15 => {
            v.push((PId::Decay, "DECAY", 100.0));
            v.push((PId::Tone, "TONE", 100.0));
        }
        _ => {
            v.push((PId::Tune, "TUNE", 100.0));
            v.push((PId::Decay, "DECAY", 100.0));
        }
    }
    v.push((PId::Level, "LEVEL", 125.0));
    v.push((PId::Pan, "PAN", 100.0));
    v
}

// ============================================================================
//  布局
// ============================================================================
unsafe fn layout_top() {
    let y = 12;
    let h = 40;
    let mut x = 176;
    G_RC_PLAY = RECT::new(x, y, x + 78, y + h);
    x += 84;
    G_RC_STOP = RECT::new(x, y, x + 78, y + h);
    x += 92;
    G_RC_BPM_M = RECT::new(x, y + 8, x + 22, y + 32);
    G_RC_BPM_V = RECT::new(x + 24, y + 8, x + 86, y + 32);
    G_RC_BPM_P = RECT::new(x + 88, y + 8, x + 110, y + 32);
    x += 118;
    G_RC_TAP = RECT::new(x, y, x + 48, y + h);
    x += 56;
    G_RC_SW_M = RECT::new(x, y + 8, x + 22, y + 32);
    G_RC_SW_V = RECT::new(x + 24, y + 8, x + 86, y + 32);
    G_RC_SW_P = RECT::new(x + 88, y + 8, x + 110, y + 32);
    x += 118;
    G_RC_PAT_M = RECT::new(x, y + 8, x + 22, y + 32);
    G_RC_PAT_V = RECT::new(x + 24, y + 8, x + 118, y + 32);
    G_RC_PAT_P = RECT::new(x + 120, y + 8, x + 142, y + 32);
    x += 150;
    G_RC_ST_M = RECT::new(x, y + 8, x + 22, y + 32);
    G_RC_ST_V = RECT::new(x + 24, y + 8, x + 74, y + 32);
    G_RC_ST_P = RECT::new(x + 76, y + 8, x + 98, y + 32);
    x += 108;
    G_RC_SONG = RECT::new(x, y, x + 66, y + h);
    x += 72;
    G_RC_CLOCK = RECT::new(x, y, x + 76, y + h);
    x += 84;
    G_RC_SAVE = RECT::new(x, y, x + 56, y + h);
    x += 60;
    G_RC_LOAD = RECT::new(x, y, x + 56, y + h);
    x += 60;
    G_RC_WAV = RECT::new(x, y, x + 76, y + h);
    x += 80;
    G_RC_MIDI = RECT::new(x, y, x + 80, y + h);
    x += 84;
    G_RC_SONGED = RECT::new(x, y, x + 74, y + h); // 歌曲链编辑（1342..1416，仍在 1500 客户区内）
    // 拍号小标签放在 MASTER 面板标题右侧：左键循环拍数，Shift+左键切换 /4 与 /8
    G_RC_SIG = RECT::new(GX + 250, MASTER_Y + 2, GX + 250 + 120, MASTER_Y + 20);
}

unsafe fn layout_grid(hwnd: HWND) {
    if G_ENG.is_null() {
        return; // 窗口创建期（WM_SIZE 早于引擎初始化）不做事
    }
    let mut rc = RECT::default();
    GetClientRect(hwnd, &mut rc);
    let avail = rc.right - 2 * GX - LABEL_W;
    // 网格列数取“整体步数”和“最长轨长度”的较大者：
    // 否则把某轨 track_len 设到 20 而 steps=16 时，第 17..20 格看不见也点不到。
    let cur = eng().proj.cur();
    let pat = &eng().proj.patterns[cur];
    let mut steps = pat.steps.max(1);
    for t in 0..crate::project::PADS {
        let tl = pat.track_len[t] as i32;
        if tl > steps {
            steps = tl;
        }
    }
    if steps > STEPS_MAX as i32 {
        steps = STEPS_MAX as i32;
    }
    G_STEPS = steps;
    let mut cw = avail / steps;
    if cw > 32 {
        cw = 32;
    }
    if cw < 8 {
        cw = 8;
    }
    G_CELL_W = cw;
}

// ============================================================================
//  绘制
// ============================================================================
unsafe fn paint(dc: HDC, w: i32, h: i32) {
    let cur = eng().proj.cur();
    let playing = SHARED.playing.load(std::sync::atomic::Ordering::Relaxed);
    let step_index = SHARED.step_index.load(std::sync::atomic::Ordering::Relaxed);
    let cell_w = G_CELL_W;
    let steps = G_STEPS;

    fill(dc, RECT::new(0, 0, w, h), C_BG);

    // ---------------- 顶部走带条 ----------------
    fill(dc, RECT::new(0, 0, w, TOP_H), C_PANEL);
    text_at(dc, 10, 2, 150, 36, "XU-808", C_ORANGE, G_FONT_LOGO, DT_LEFT);
    text_at(dc, 12, 40, 170, 16, "RHYTHM COMPOSER · 徐某 · 16 VOICE", C_DIM, G_FONT_TINY, DT_LEFT);

    draw_btn(dc, G_RC_PLAY, "▶ PLAY", playing);
    draw_btn(dc, G_RC_STOP, "■ STOP", false);
    draw_btn(dc, G_RC_TAP, "TAP", false);

    {
        let m = eng().proj.master;
        draw_btn(dc, G_RC_BPM_M, "-", false);
        text_at(
            dc,
            G_RC_BPM_V.left,
            G_RC_BPM_V.top,
            G_RC_BPM_V.w(),
            24,
            &format!("{:.1}", m.bpm),
            C_ORANGE,
            G_FONT_BOLD,
            DT_CENTER,
        );
        draw_btn(dc, G_RC_BPM_P, "+", false);
        text_at(dc, G_RC_BPM_M.left, 1, 110, 12, "BPM", C_DIM, G_FONT_TINY, DT_LEFT);

        draw_btn(dc, G_RC_SW_M, "-", false);
        text_at(
            dc,
            G_RC_SW_V.left,
            G_RC_SW_V.top,
            G_RC_SW_V.w(),
            24,
            &format!("{}%", (m.swing * 100.0) as i32),
            C_ORANGE,
            G_FONT_BOLD,
            DT_CENTER,
        );
        draw_btn(dc, G_RC_SW_P, "+", false);
        text_at(dc, G_RC_SW_M.left, 1, 110, 12, "SWING", C_DIM, G_FONT_TINY, DT_LEFT);
    }

    {
        draw_btn(dc, G_RC_PAT_M, "<", false);
        let name = eng().proj.patterns[cur].name.clone();
        text_at(
            dc,
            G_RC_PAT_V.left,
            G_RC_PAT_V.top,
            G_RC_PAT_V.w(),
            24,
            &format!("{:02} {}", cur + 1, name),
            C_TEXT,
            G_FONT_NORM,
            DT_CENTER,
        );
        draw_btn(dc, G_RC_PAT_P, ">", false);
        text_at(dc, G_RC_PAT_M.left, 1, 110, 12, "PATTERN", C_DIM, G_FONT_TINY, DT_LEFT);
    }

    {
        let st = eng().proj.patterns[cur].steps;
        draw_btn(dc, G_RC_ST_M, "-", false);
        text_at(dc, G_RC_ST_V.left, G_RC_ST_V.top, G_RC_ST_V.w(), 24, &st.to_string(), C_ORANGE, G_FONT_BOLD, DT_CENTER);
        draw_btn(dc, G_RC_ST_P, "+", false);
        text_at(dc, G_RC_ST_M.left, 1, 80, 12, "STEPS", C_DIM, G_FONT_TINY, DT_LEFT);
    }

    draw_btn(dc, G_RC_SONG, "SONG", eng().proj.master.song_mode);
    draw_btn(dc, G_RC_CLOCK, "MIDI CLK", eng().clock_external);
    draw_btn(dc, G_RC_SAVE, "保存", false);
    draw_btn(dc, G_RC_LOAD, "打开", false);
    draw_btn(dc, G_RC_WAV, "导出WAV", false);
    draw_btn(dc, G_RC_MIDI, "导出MIDI", false);
    {
        // 歌曲链条数直接显示在按钮上，一眼能看出有没有编排过整曲
        let n = eng().proj.song.len();
        let label = if n > 0 { format!("歌曲链{}", n) } else { "歌曲链".to_string() };
        draw_btn(dc, G_RC_SONGED, &label, eng().proj.master.song_mode);
    }

    // ---------------- 步进网格 ----------------
    let gy = GY;
    let grid_w = LABEL_W + steps * cell_w;
    fill(dc, RECT::new(GX, gy, GX + grid_w, gy + HEAD_H), C_PANEL);
    for c in 0..steps {
        let x = GX + LABEL_W + c * cell_w;
        if c % 4 == 0 {
            fill(dc, RECT::new(x, gy, x + 1, gy + HEAD_H + GRID_ROWS * CELL_H), C_BEAT_LINE);
        }
        text_at(dc, x, gy, cell_w, HEAD_H, &(c + 1).to_string(), C_DIM, G_FONT_TINY, DT_CENTER);
    }
    fill(dc, RECT::new(GX, gy + HEAD_H, GX + LABEL_W, gy + HEAD_H + GRID_ROWS * CELL_H), C_PANEL2);

    for r in 0..GRID_ROWS as usize {
        let y = gy + HEAD_H + r as i32 * CELL_H;
        fill(
            dc,
            RECT::new(GX + LABEL_W, y, GX + grid_w, y + CELL_H),
            if r % 2 == 0 { C_ROW_A } else { C_ROW_B },
        );
        text_at(dc, GX + 2, y, 26, CELL_H, crate::project::CODES[r], C_ORANGE, G_FONT_MONO, DT_LEFT);
        text_at(dc, GX + 28, y, 48, CELL_H, crate::project::NAMES_CN[r], C_TEXT, G_FONT_TINY, DT_LEFT);
        // M / S 按钮
        for j in 0..2 {
            let on = if j == 0 { eng().proj.pads[r].mute } else { eng().proj.pads[r].solo };
            let mb = RECT::new(GX + 78 + j * 16, y + 4, GX + 93 + j * 16, y + CELL_H - 4);
            fill(dc, mb, if on { if j == 0 { C_RED } else { C_YELLOW } } else { rgb(42, 42, 42) });
            text_at(
                dc,
                mb.left,
                mb.top,
                15,
                mb.h(),
                if j == 0 { "M" } else { "S" },
                if on { C_BG } else { C_DIM },
                G_FONT_TINY,
                DT_CENTER,
            );
        }
        // 每轨独立长度（polymeter）：[-] 数值 [+]，范围 1..64
        // 与整体 steps 无关：轨道按 global_step % track_len 取格，短轨会自动循环
        let tl = eng().proj.patterns[cur].track_len[r as usize];
        let tm = RECT::new(GX + 112, y + 4, GX + 126, y + CELL_H - 4);
        let tp = RECT::new(GX + 152, y + 4, GX + 166, y + CELL_H - 4);
        draw_btn(dc, tm, "-", false);
        draw_btn(dc, tp, "+", false);
        text_at(
            dc,
            GX + 126,
            y,
            26,
            CELL_H,
            &format!("{:02}", tl),
            if tl as i32 != steps { C_CYAN } else { C_DIM },
            G_FONT_TINY,
            DT_CENTER,
        );
        // 步进格
        for c in 0..steps as usize {
            let st = eng().proj.patterns[cur].data[r][c];
            let cell = RECT::new(
                GX + LABEL_W + c as i32 * cell_w + 2,
                y + 2,
                GX + LABEL_W + (c as i32 + 1) * cell_w - 2,
                y + CELL_H - 2,
            );
            if st.on {
                let a = 110 + (145.0 * (st.velocity as f32 / 127.0)) as i32;
                let base = if st.accent { C_YELLOW } else { C_ORANGE };
                let mix = |ch: u32| -> u32 {
                    let bc = if ch == 0 { base & 0xff } else if ch == 1 { (base >> 8) & 0xff } else { (base >> 16) & 0xff };
                    (bc * a as u32 + 18 * (255 - a) as u32) / 255
                };
                fill(dc, cell, rgb(mix(0), mix(1), mix(2)));
                if st.prob < 100 || st.ratchet > 1 {
                    text_at(
                        dc,
                        cell.left,
                        cell.top,
                        cell.w(),
                        cell.h(),
                        if st.ratchet > 1 { "R" } else { "%" },
                        rgb(26, 16, 8),
                        G_FONT_TINY,
                        DT_CENTER,
                    );
                }
            } else {
                fill(dc, cell, C_CELL_OFF);
                frame(dc, cell, C_CELL_BORDER);
            }
            if c as i32 == G_SEL_STEP && r as i32 == G_SEL_PAD {
                frame(dc, cell, C_CYAN);
            }
        }
        // 短轨结束标记：track_len 小于整体步数时画一条竖线，直观看出本轨在哪儿循环
        if (tl as i32) < steps {
            let xe = GX + LABEL_W + tl as i32 * cell_w;
            let p = pen(rgb(90, 90, 90));
            let old = SelectObject(dc, p);
            MoveToEx(dc, xe, y, std::ptr::null_mut());
            LineTo(dc, xe, y + CELL_H);
            SelectObject(dc, old);
        }
    }

    // 播放头（cyan 双线）
    if playing {
        let c = step_index;
        if c >= 0 && c < steps {
            let x = GX + LABEL_W + c * cell_w;
            let p = pen(C_CYAN);
            let old = SelectObject(dc, p);
            MoveToEx(dc, x, gy + HEAD_H, std::ptr::null_mut());
            LineTo(dc, x, gy + HEAD_H + GRID_ROWS * CELL_H);
            MoveToEx(dc, x + cell_w, gy + HEAD_H, std::ptr::null_mut());
            LineTo(dc, x + cell_w, gy + HEAD_H + GRID_ROWS * CELL_H);
            SelectObject(dc, old);
        }
    }

    // ---------------- 步进属性条 ----------------
    {
        let y = STEP_ED_Y;
        fill(dc, RECT::new(GX, y, w - GX, y + STEP_ED_H), C_PANEL);
        let pad = G_SEL_PAD as usize;
        let stp = G_SEL_STEP as usize;
        let st = eng().proj.patterns[cur].data[pad][stp];
        text_at(
            dc,
            GX + 8,
            y + 4,
            160,
            20,
            &format!("STEP  {} {:02}", crate::project::CODES[pad], stp + 1),
            C_ORANGE,
            G_FONT_BOLD,
            DT_LEFT,
        );

        let labels = ["ON", "ACC", "VEL", "PROB", "MIC", "RAT", "COND"];
        let vals = [
            st.on as i32,
            st.accent as i32,
            st.velocity as i32,
            st.prob as i32,
            st.micro as i32,
            st.ratchet as i32,
            st.cond as i32,
        ];
        let mut x = GX + 8;
        for i in 0..7 {
            let box_w = if i == 6 { 140 } else { 112 };
            G_RC_STEPED[i][0] = RECT::new(x, y + 30, x + 20, y + 50);
            G_RC_STEPED[i][1] = RECT::new(x + 22, y + 30, x + box_w - 22, y + 50);
            G_RC_STEPED[i][2] = RECT::new(x + box_w - 20, y + 30, x + box_w, y + 50);
            text_at(dc, x, y + 14, box_w, 12, labels[i], C_DIM, G_FONT_TINY, DT_LEFT);
            draw_btn(dc, G_RC_STEPED[i][0], "-", false);
            draw_btn(dc, G_RC_STEPED[i][2], "+", false);
            let txt = if i == 6 {
                match vals[6] {
                    0 => "总是".to_string(),
                    1 => "过门".to_string(),
                    2 => "非过门".to_string(),
                    3 => "每2".to_string(),
                    _ => "每4".to_string(),
                }
            } else {
                vals[i].to_string()
            };
            text_at(
                dc,
                G_RC_STEPED[i][1].left,
                G_RC_STEPED[i][1].top,
                G_RC_STEPED[i][1].w(),
                20,
                &txt,
                C_TEXT,
                G_FONT_BOLD,
                DT_CENTER,
            );
            x += box_w + 12;
        }
    }

    // ---------------- 声部参数 ----------------
    {
        let y = PARAM_Y;
        fill(dc, RECT::new(GX, y, w - GX, y + PARAM_H), C_PANEL);
        let pad = G_SEL_PAD as usize;
        text_at(
            dc,
            GX + 8,
            y + 2,
            260,
            16,
            &format!("PAD {:02}  {}  {}", pad, crate::project::CODES[pad], crate::project::NAMES_CN[pad]),
            C_ORANGE,
            G_FONT_BOLD,
            DT_LEFT,
        );
        let params = param_defs(G_SEL_PAD);
        let mut x = GX + 8;
        for (id, label, mx) in params.iter() {
            let p = eng().proj.pads[pad];
            let v = pget(&p, *id);
            let v01 = if *mx > 0.0 { v / *mx } else { 0.0 };
            draw_slider(dc, x, y + 22, 86, label, v01.clamp(0.0, 1.0));
            text_at(dc, x, y + 60, 86, 14, &format!("{}", (v * 100.0) as i32), C_ORANGE, G_FONT_TINY, DT_LEFT);
            x += 94;
            if x > w - 120 {
                break;
            }
        }
        let p = eng().proj.pads[pad];
        let mut flags = String::new();
        if p.mute {
            flags.push_str("MUTE ");
        }
        if p.solo {
            flags.push_str("SOLO");
        }
        text_at(dc, w - 200, y + 4, 180, 16, &flags, C_YELLOW, G_FONT_SMALL, DT_RIGHT);
    }

    // ---------------- 主效果 + 电平表 ----------------
    {
        let y = MASTER_Y;
        fill(dc, RECT::new(GX, y, w - GX, y + MASTER_H), C_PANEL);
        text_at(dc, GX + 8, y + 2, 240, 16, "MASTER · 徐某的母线", C_ORANGE, G_FONT_BOLD, DT_LEFT);
        let m = eng().proj.master;
        // 拍号（可点击）：TIME 4/4
        text_at(
            dc,
            G_RC_SIG.left,
            G_RC_SIG.top,
            G_RC_SIG.w(),
            G_RC_SIG.h(),
            &format!("TIME {}/{}", m.beats_per_bar, m.beat_unit),
            C_DIM,
            G_FONT_TINY,
            DT_LEFT,
        );
        let labels = ["LP 低通", "RES 共鸣", "HP 高通", "DRIVE 过载", "VOL 音量"];
        let vals = [
            (m.lp_cutoff / 20000.0, m.lp_cutoff as i32),
            (m.lp_q / 8.0, (m.lp_q * 100.0) as i32),
            (m.hp_cutoff / 800.0, m.hp_cutoff as i32),
            (m.drive / 8.0, (m.drive * 100.0) as i32),
            (m.volume, (m.volume * 100.0) as i32),
        ];
        let mut x = GX + 8;
        for i in 0..5 {
            draw_slider(dc, x, y + 22, 130, labels[i], vals[i].0.clamp(0.0, 1.0));
            text_at(dc, x, y + 60, 130, 14, &vals[i].1.to_string(), C_ORANGE, G_FONT_TINY, DT_LEFT);
            // LP / HP 滤波器开关：直接读写 master.lp_on / hp_on
            if i == 0 || i == 2 {
                let on = if i == 0 { m.lp_on } else { m.hp_on };
                let tr = RECT::new(x + 96, y + 20, x + 128, y + 37);
                draw_btn(dc, tr, if on { "ON" } else { "OFF" }, on);
                if i == 0 {
                    G_RC_LP_ON = tr;
                } else {
                    G_RC_HP_ON = tr;
                }
            }
            x += 142;
        }
        // 电平表
        let mx = w - 330;
        fill(dc, RECT::new(mx, y + 24, mx + 160, y + 34), rgb(12, 12, 12));
        let (ml, mr) = SHARED.meter();
        let lw = (ml.clamp(0.0, 1.0) * 156.0) as i32;
        fill(dc, RECT::new(mx + 2, y + 26, mx + 2 + lw, y + 32), C_GREEN);
        fill(dc, RECT::new(mx, y + 40, mx + 160, y + 50), rgb(12, 12, 12));
        let rw = (mr.clamp(0.0, 1.0) * 156.0) as i32;
        fill(dc, RECT::new(mx + 2, y + 42, mx + 2 + rw, y + 48), C_GREEN);

        // 音频状态：初始化可能在后台线程里进行，先把“启动中”显示出来
        let audio_txt = match audio::init_state() {
            1 => "音频启动中",
            3 => "无音频",
            _ => {
                if SHARED.audio_ok.load(std::sync::atomic::Ordering::Relaxed) {
                    "48kHz"
                } else {
                    "无音频"
                }
            }
        };
        let status = format!(
            "CPU {:.1}%   XRUN {}   MIDI {}   {}",
            SHARED.cpu() * 100.0,
            SHARED.xruns.load(std::sync::atomic::Ordering::Relaxed),
            if SHARED.midi_on.load(std::sync::atomic::Ordering::Relaxed) { "ON" } else { "-" },
            audio_txt
        );
        text_at(dc, w - 330, y + 58, 320, 16, &status, C_DIM, G_FONT_TINY, DT_RIGHT);
    }

    let _ = h;
}

// ============================================================================
//  文件对话框
// ============================================================================
unsafe fn ask_save(file: &mut Vec<u16>, filter: &[u16], def_ext: &[u16]) -> bool {
    let mut ofn = OPENFILENAMEW::default();
    ofn.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
    ofn.hwndOwner = G_HWND;
    ofn.lpstrFilter = filter.as_ptr();
    ofn.lpstrFile = file.as_mut_ptr();
    ofn.nMaxFile = file.len() as u32;
    ofn.lpstrDefExt = def_ext.as_ptr();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    GetSaveFileNameW(&mut ofn) != 0
}

unsafe fn ask_open(file: &mut Vec<u16>, filter: &[u16]) -> bool {
    let mut ofn = OPENFILENAMEW::default();
    ofn.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
    ofn.hwndOwner = G_HWND;
    ofn.lpstrFilter = filter.as_ptr();
    ofn.lpstrFile = file.as_mut_ptr();
    ofn.nMaxFile = file.len() as u32;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    GetOpenFileNameW(&mut ofn) != 0
}

/// UTF-16 缓冲（含 0）→ String
fn u16_to_string(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}

/// 构造“过滤器”双 \0 串
fn filter_string(items: &[(&str, &str)]) -> Vec<u16> {
    let mut v: Vec<u16> = Vec::new();
    for (name, pat) in items {
        v.extend(name.encode_utf16());
        v.push(0);
        v.extend(pat.encode_utf16());
        v.push(0);
    }
    v.push(0);
    v
}

// ============================================================================
//  交互
// ============================================================================
unsafe fn step_edit_adjust(idx: usize, delta: i32) {
    let cur = eng().proj.cur();
    let pad = G_SEL_PAD as usize;
    let stp = G_SEL_STEP as usize;
    let st = &mut eng().proj.patterns[cur].data[pad][stp];
    match idx {
        0 => st.on = !st.on,
        1 => st.accent = !st.accent,
        2 => st.velocity = (st.velocity as i32 + delta).clamp(1, 127) as u8,
        3 => st.prob = (st.prob as i32 + delta).clamp(0, 100) as u8,
        4 => st.micro = (st.micro as i32 + delta * 10).clamp(-2400, 2400) as i16,
        5 => st.ratchet = (st.ratchet as i32 + delta).clamp(1, 8) as u8,
        6 => st.cond = (((st.cond as i32 + delta) % 5 + 5) % 5) as u8,
        _ => {}
    }
    InvalidateRect(G_HWND, std::ptr::null(), 0);
}

/// Tap Tempo：最近 8 次间隔，去掉最大最小后平均；异常值丢弃
unsafe fn tap_tempo() {
    static mut LAST: u64 = 0;
    static mut IV: [f64; 8] = [0.0; 8];
    static mut N: usize = 0;

    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    if LAST != 0 && now.saturating_sub(LAST) < 2000 {
        let dt = (now - LAST) as f64 / 1000.0;
        if N < 8 {
            IV[N] = dt;
            N += 1;
        } else {
            for i in 1..8 {
                IV[i - 1] = IV[i];
            }
            IV[7] = dt;
        }
        if N >= 2 {
            let mut sum = 0.0;
            let mut cnt = 0;
            let mut mn = f64::MAX;
            let mut mx = 0.0f64;
            for i in 0..N {
                mn = mn.min(IV[i]);
                mx = mx.max(IV[i]);
                sum += IV[i];
                cnt += 1;
            }
            let mean = if N >= 3 { (sum - mn - mx) / (cnt as f64 - 2.0) } else { sum / cnt as f64 };
            if mean > 0.0 {
                let bpm = 60.0 / mean;
                if (40.0..=300.0).contains(&bpm) {
                    eng().proj.master.bpm = bpm as f32;
                }
            }
        }
    } else {
        N = 0;
    }
    LAST = now;
    InvalidateRect(G_HWND, std::ptr::null(), 0);
}

unsafe fn on_lbutton(x: i32, y: i32) {
    let cur = eng().proj.cur();

    // ---- 顶部按钮 ----
    if G_RC_PLAY.inside(x, y) {
        eng().play();
        return;
    }
    if G_RC_STOP.inside(x, y) {
        eng().stop();
        return;
    }
    if G_RC_TAP.inside(x, y) {
        tap_tempo();
        return;
    }
    if G_RC_BPM_M.inside(x, y) {
        let b = (eng().proj.master.bpm - 1.0).max(40.0);
        eng().proj.master.bpm = b;
        return;
    }
    if G_RC_BPM_P.inside(x, y) {
        let b = (eng().proj.master.bpm + 1.0).min(300.0);
        eng().proj.master.bpm = b;
        return;
    }
    if G_RC_SW_M.inside(x, y) {
        let s = (eng().proj.master.swing - 0.01).max(0.0);
        eng().proj.master.swing = s;
        return;
    }
    if G_RC_SW_P.inside(x, y) {
        let s = (eng().proj.master.swing + 0.01).min(0.75);
        eng().proj.master.swing = s;
        return;
    }
    if G_RC_PAT_M.inside(x, y) {
        let n = (eng().proj.current - 1 + PATTERNS as i32) % PATTERNS as i32;
        eng().request_pattern(n);
        layout_grid(G_HWND);
        return;
    }
    if G_RC_PAT_P.inside(x, y) {
        let n = (eng().proj.current + 1) % PATTERNS as i32;
        eng().request_pattern(n);
        layout_grid(G_HWND);
        return;
    }
    if G_RC_ST_M.inside(x, y) {
        let st = &mut eng().proj.patterns[cur].steps;
        if *st > 1 {
            *st -= 1;
        }
        layout_grid(G_HWND);
        return;
    }
    if G_RC_ST_P.inside(x, y) {
        let st = &mut eng().proj.patterns[cur].steps;
        if *st < STEPS_MAX as i32 {
            *st += 1;
        }
        layout_grid(G_HWND);
        return;
    }
    if G_RC_SONG.inside(x, y) {
        let shift = GetKeyState(VK_SHIFT) < 0;
        if shift {
            // Shift+SONG：快速把当前 pattern 追加到歌曲链末尾（默认重复 1 次，
            // 具体次数到“歌曲链”窗口里改）
            let cur = eng().proj.current;
            if eng().proj.song.len() < 64 {
                eng().proj.song.push(crate::project::SongEntry { pattern: cur, repeats: 1 });
            }
        } else {
            eng().proj.master.song_mode = !eng().proj.master.song_mode;
        }
        return;
    }
    if G_RC_SONGED.inside(x, y) {
        // 打开歌曲链编辑窗口（P-/P+ 改 pattern，R-/R+ 改 repeats，可增删/排序）
        open_song_editor();
        return;
    }
    if G_RC_SIG.inside(x, y) {
        // 拍号：左键循环 1..16 拍，Shift+左键在 /4 与 /8 之间切换
        if GetKeyState(VK_SHIFT) < 0 {
            eng().proj.master.beat_unit = if eng().proj.master.beat_unit == 8 { 4 } else { 8 };
        } else {
            let b = eng().proj.master.beats_per_bar + 1;
            eng().proj.master.beats_per_bar = if b > 16 { 1 } else { b };
        }
        return;
    }
    if G_RC_CLOCK.inside(x, y) {
        let v = !eng().clock_external;
        eng().clock_external = v;
        return;
    }

    // ---- 保存 / 打开 / 导出 ----
    if G_RC_SAVE.inside(x, y) {
        let filter = filter_string(&[("XU-808 工程", "*.xu808"), ("JSON", "*.json"), ("所有文件", "*.*")]);
        let ext = wide("xu808");
        let mut file = vec![0u16; MAX_PATH];
        for (i, c) in "xu808.xu808".encode_utf16().enumerate() {
            file[i] = c;
        }
        if ask_save(&mut file, &filter, &ext) {
            let path = u16_to_string(&file);
            let _ = eng().proj.save_json(&path);
        }
        return;
    }
    if G_RC_LOAD.inside(x, y) {
        let filter = filter_string(&[("XU-808 工程", "*.xu808"), ("JSON", "*.json"), ("所有文件", "*.*")]);
        let mut file = vec![0u16; MAX_PATH];
        if ask_open(&mut file, &filter) {
            let path = u16_to_string(&file);
            // 先停音频再换工程：回调线程正在读老引擎，直接替换会读到半截数据
            audio::stop();
            eng().stop();
            if let Ok(np) = Project::load_json(&path) {
                let old = G_ENG;
                let ne = Box::into_raw(Box::new(Engine::new(np)));
                G_ENG = ne;
                drop(Box::from_raw(old));
            }
            // 同样把设备重开放到后台线程，避免打开失败/阻塞时 UI 假死
            let addr = G_ENG as usize;
            std::thread::spawn(move || unsafe {
                audio::start(addr as *mut Engine);
                midi::start();
            });
            layout_grid(G_HWND);
        }
        return;
    }
    if G_RC_WAV.inside(x, y) {
        let filter = filter_string(&[("WAV 音频", "*.wav")]);
        let ext = wide("wav");
        let mut file = vec![0u16; MAX_PATH];
        for (i, c) in "xu808.wav".encode_utf16().enumerate() {
            file[i] = c;
        }
        if ask_save(&mut file, &filter, &ext) {
            let path = u16_to_string(&file);
            let song = eng().proj.master.song_mode;
            let stems = GetKeyState(VK_SHIFT) < 0; // Shift = 分轨导出
            SetCursor(LoadCursorW(std::ptr::null_mut(), make_int_resource(IDC_WAIT)));
            if stems {
                // 分轨：每轨单独渲染一个 WAV（soloOverride），文件名带声部代号
                let base = path
                    .strip_suffix(".wav")
                    .or_else(|| path.strip_suffix(".WAV"))
                    .unwrap_or(&path)
                    .to_string();
                for pad in 0..crate::project::PADS {
                    let p = format!("{}_{}.wav", base, crate::project::CODES[pad]);
                    let _ = export::export_wav(&eng().proj, &p, song, pad as i32);
                }
            } else {
                let _ = export::export_wav(&eng().proj, &path, song, -1);
            }
            SetCursor(LoadCursorW(std::ptr::null_mut(), make_int_resource(IDC_ARROW)));
        }
        return;
    }
    if G_RC_MIDI.inside(x, y) {
        let filter = filter_string(&[("MIDI", "*.mid")]);
        let ext = wide("mid");
        let mut file = vec![0u16; MAX_PATH];
        for (i, c) in "xu808.mid".encode_utf16().enumerate() {
            file[i] = c;
        }
        if ask_save(&mut file, &filter, &ext) {
            let path = u16_to_string(&file);
            let song = eng().proj.master.song_mode;
            let _ = export::export_midi(&eng().proj, &path, song);
        }
        return;
    }

    // ---- 步进属性条 ----
    for i in 0..7 {
        if G_RC_STEPED[i][0].inside(x, y) {
            step_edit_adjust(i, -1);
            return;
        }
        if G_RC_STEPED[i][2].inside(x, y) {
            step_edit_adjust(i, 1);
            return;
        }
        if G_RC_STEPED[i][1].inside(x, y) && (i == 0 || i == 1) {
            step_edit_adjust(i, 1);
            return;
        }
    }

    // ---- 左侧声部名 / M / S / 每轨长度 ----
    let gy = GY + HEAD_H;
    if x >= GX && x < GX + LABEL_W && y >= gy && y < gy + GRID_ROWS * CELL_H {
        let r = (y - gy) / CELL_H;
        let row_y = gy + r * CELL_H;
        // 每轨长度（polymeter）的 -/+：优先于 M/S 命中判定
        let tm = RECT::new(GX + 112, row_y + 4, GX + 126, row_y + CELL_H - 4);
        let tp = RECT::new(GX + 152, row_y + 4, GX + 166, row_y + CELL_H - 4);
        if tm.inside(x, y) || tp.inside(x, y) {
            let d = if tp.inside(x, y) { 1 } else { -1 };
            let tl = &mut eng().proj.patterns[cur].track_len[r as usize];
            *tl = (*tl as i32 + d).clamp(1, STEPS_MAX as i32) as u8;
            layout_grid(G_HWND); // 轨长可能超过 steps，需要重算网格列数
            InvalidateRect(G_HWND, std::ptr::null(), 0);
            return;
        }
        for j in 0..2 {
            let mb = RECT::new(GX + 78 + j * 16, gy + r * CELL_H + 4, GX + 93 + j * 16, gy + r * CELL_H + CELL_H - 4);
            if mb.inside(x, y) {
                if j == 0 {
                    eng().proj.pads[r as usize].mute = !eng().proj.pads[r as usize].mute;
                } else {
                    eng().proj.pads[r as usize].solo = !eng().proj.pads[r as usize].solo;
                }
                InvalidateRect(G_HWND, std::ptr::null(), 0);
                return;
            }
        }
        G_SEL_PAD = r;
        ui_trigger_pad(r, 112, false);
        InvalidateRect(G_HWND, std::ptr::null(), 0);
        return;
    }

    // ---- 步进格：左键开关 ----
    let gx = GX + LABEL_W;
    if x >= gx && x < gx + G_STEPS * G_CELL_W && y >= gy && y < gy + GRID_ROWS * CELL_H {
        let c = (x - gx) / G_CELL_W;
        let r = (y - gy) / CELL_H;
        if c >= 0 && c < G_STEPS && r >= 0 && r < GRID_ROWS {
            let st = &mut eng().proj.patterns[cur].data[r as usize][c as usize];
            st.on = !st.on;
            let on = st.on;
            let vel = st.velocity;
            let acc = st.accent;
            G_SEL_PAD = r;
            G_SEL_STEP = c;
            if on {
                ui_trigger_pad(r, vel as i32, acc);
            }
            InvalidateRect(G_HWND, std::ptr::null(), 0);
        }
        return;
    }

    // ---- 声部参数滑杆 ----
    if y >= PARAM_Y && y < PARAM_Y + PARAM_H {
        let params = param_defs(G_SEL_PAD);
        let mut px = GX + 8;
        for (i, (id, _, mx)) in params.iter().enumerate() {
            let tr = RECT::new(px, PARAM_Y + 42, px + 86, PARAM_Y + 50);
            if x >= px && x < px + 86 && y >= PARAM_Y + 16 && y < PARAM_Y + 60 {
                let v = slider_value(tr, x) * *mx;
                let pad = G_SEL_PAD as usize;
                pset(&mut eng().proj.pads[pad], *id, v);
                G_DRAG_PARAM = i as i32;
                SetCapture(G_HWND);
                InvalidateRect(G_HWND, std::ptr::null(), 0);
                return;
            }
            px += 94;
        }
    }

    // ---- 主效果滑杆 ----
    if y >= MASTER_Y && y < MASTER_Y + MASTER_H {
        // 先判 LP / HP 开关按钮，再判滑杆
        if G_RC_LP_ON.inside(x, y) {
            eng().proj.master.lp_on = !eng().proj.master.lp_on;
            InvalidateRect(G_HWND, std::ptr::null(), 0);
            return;
        }
        if G_RC_HP_ON.inside(x, y) {
            eng().proj.master.hp_on = !eng().proj.master.hp_on;
            InvalidateRect(G_HWND, std::ptr::null(), 0);
            return;
        }
        let mut px = GX + 8;
        for i in 0..5 {
            let tr = RECT::new(px, MASTER_Y + 42, px + 130, MASTER_Y + 50);
            if x >= px && x < px + 130 && y >= MASTER_Y + 16 && y < MASTER_Y + 60 {
                let v = slider_value(tr, x);
                match i {
                    0 => eng().proj.master.lp_cutoff = 200.0 + v * 19800.0,
                    1 => eng().proj.master.lp_q = 0.5 + v * 7.5,
                    2 => eng().proj.master.hp_cutoff = 10.0 + v * 790.0,
                    3 => eng().proj.master.drive = 1.0 + v * 7.0,
                    _ => eng().proj.master.volume = v,
                }
                G_DRAG_MASTER = i;
                SetCapture(G_HWND);
                InvalidateRect(G_HWND, std::ptr::null(), 0);
                return;
            }
            px += 142;
        }
    }
}

unsafe fn on_rbutton(x: i32, y: i32) {
    let gy = GY + HEAD_H;
    let gx = GX + LABEL_W;
    if x >= gx && x < gx + G_STEPS * G_CELL_W && y >= gy && y < gy + GRID_ROWS * CELL_H {
        let c = (x - gx) / G_CELL_W;
        let r = (y - gy) / CELL_H;
        if c >= 0 && c < G_STEPS && r >= 0 && r < GRID_ROWS {
            // 右键 = 选中该格，然后在下方属性条里细调
            G_SEL_PAD = r;
            G_SEL_STEP = c;
            InvalidateRect(G_HWND, std::ptr::null(), 0);
        }
    }
}

unsafe fn on_mouse_move(x: i32, _y: i32) {
    if G_DRAG_PARAM >= 0 {
        let params = param_defs(G_SEL_PAD);
        let i = G_DRAG_PARAM as usize;
        if i < params.len() {
            let px = GX + 8 + G_DRAG_PARAM * 94;
            let tr = RECT::new(px, PARAM_Y + 42, px + 86, PARAM_Y + 50);
            let (id, _, mx) = params[i];
            let v = slider_value(tr, x) * mx;
            let pad = G_SEL_PAD as usize;
            pset(&mut eng().proj.pads[pad], id, v);
            InvalidateRect(G_HWND, std::ptr::null(), 0);
        }
    } else if G_DRAG_MASTER >= 0 {
        let px = GX + 8 + G_DRAG_MASTER * 142;
        let tr = RECT::new(px, MASTER_Y + 42, px + 130, MASTER_Y + 50);
        let v = slider_value(tr, x);
        match G_DRAG_MASTER {
            0 => eng().proj.master.lp_cutoff = 200.0 + v * 19800.0,
            1 => eng().proj.master.lp_q = 0.5 + v * 7.5,
            2 => eng().proj.master.hp_cutoff = 10.0 + v * 790.0,
            3 => eng().proj.master.drive = 1.0 + v * 7.0,
            _ => eng().proj.master.volume = v,
        }
        InvalidateRect(G_HWND, std::ptr::null(), 0);
    }
}

// ============================================================================
//  歌曲链编辑窗口（作者：徐某）
//
//  规格要求“歌曲链必须能编辑 pattern 与 repeats”，且不能写死 4 次。
//  这里用一个非模态弹窗实现：
//    · 每行：序号 / Pattern 名 / P- P+ / 重复次数 / R- R+ / 上移 / 下移 / 删除；
//    · 底部：追加当前 Pattern、清空、关闭；
//    · 右侧：▲/▼ 翻页（最多 64 条，一屏显示 10 条）。
//  直接读写引擎里的 proj.song，改完立即生效（SONG 模式下按新顺序播放）。
// ============================================================================
/// 把选中项与滚动位置夹回合法范围
unsafe fn song_clamp() {
    let n = eng().proj.song.len();
    if n == 0 {
        G_SONG_SEL = 0;
        G_SONG_TOP = 0;
        return;
    }
    if G_SONG_SEL >= n {
        G_SONG_SEL = n - 1;
    }
    if G_SONG_TOP + SG_ROWS > n && n > SG_ROWS {
        G_SONG_TOP = n - SG_ROWS;
    }
    if G_SONG_TOP >= n {
        G_SONG_TOP = 0;
    }
}

/// 画一帧歌曲链窗口
unsafe fn song_paint(dc: HDC, w: i32, h: i32) {
    fill(dc, RECT::new(0, 0, w, h), C_BG);
    fill(dc, RECT::new(0, 0, w, 32), C_PANEL);
    text_at(dc, 12, 2, 420, 28, "歌曲链编辑 · 徐某", C_ORANGE, G_FONT_BOLD, DT_LEFT);
    text_at(
        dc,
        12,
        33,
        w - 24,
        12,
        "点行选中；P-/P+ 改 Pattern，R-/R+ 改重复次数（1-64）；上移/下移 调顺序",
        C_DIM,
        G_FONT_TINY,
        DT_LEFT,
    );

    let n = eng().proj.song.len();
    let top = if n > SG_ROWS { G_SONG_TOP.min(n - SG_ROWS) } else { 0 };
    let list_h = SG_ROWS as i32 * SG_ROW_H;
    fill(dc, RECT::new(12, SG_LIST_Y - 2, 12 + w - 24, SG_LIST_Y + list_h), C_PANEL);

    if n == 0 {
        text_at(
            dc,
            12,
            SG_LIST_Y + 12,
            w - 24,
            24,
            "歌曲链为空：点下方「追加当前」把当前 Pattern 加进来",
            C_DIM,
            G_FONT_SMALL,
            DT_CENTER,
        );
    }

    for i in 0..SG_ROWS {
        let y = SG_LIST_Y + i as i32 * SG_ROW_H;
        let idx = top + i;
        if idx >= n {
            // 空行：清掉命中区，避免点到上一帧残留的按钮
            for b in 0..SG_BTNS {
                G_SONG_RC[i][b] = ZRECT;
            }
            continue;
        }
        let row = RECT::new(12, y, 12 + w - 24, y + SG_ROW_H - 2);
        fill(dc, row, if idx == G_SONG_SEL { rgb(52, 38, 22) } else if i % 2 == 0 { C_ROW_A } else { C_ROW_B });
        if idx == G_SONG_SEL {
            frame(dc, row, C_ORANGE);
        }

        let e = eng().proj.song[idx];
        let pidx = e.pattern.clamp(0, PATTERNS as i32 - 1) as usize;
        text_at(dc, 16, y, 30, SG_ROW_H - 2, &format!("{:02}", idx + 1), C_DIM, G_FONT_TINY, DT_LEFT);
        let nm = eng().proj.patterns[pidx].name.clone();
        text_at(dc, 44, y, 100, SG_ROW_H - 2, &format!("{:02} {}", pidx + 1, nm), C_TEXT, G_FONT_SMALL, DT_LEFT);

        // 每行 7 个控件：P- P+ R- R+ 上移 下移 删除
        let by = y + 3;
        let bh = SG_ROW_H - 9;
        G_SONG_RC[i][0] = RECT::new(144, by, 169, by + bh);
        G_SONG_RC[i][1] = RECT::new(171, by, 196, by + bh);
        G_SONG_RC[i][2] = RECT::new(240, by, 265, by + bh);
        G_SONG_RC[i][3] = RECT::new(267, by, 292, by + bh);
        G_SONG_RC[i][4] = RECT::new(300, by, 338, by + bh);
        G_SONG_RC[i][5] = RECT::new(341, by, 379, by + bh);
        G_SONG_RC[i][6] = RECT::new(382, by, 416, by + bh);
        text_at(dc, 198, y, 40, SG_ROW_H - 2, &format!("x{}", e.repeats), C_YELLOW, G_FONT_BOLD, DT_CENTER);
        draw_btn(dc, G_SONG_RC[i][0], "P-", false);
        draw_btn(dc, G_SONG_RC[i][1], "P+", false);
        draw_btn(dc, G_SONG_RC[i][2], "R-", false);
        draw_btn(dc, G_SONG_RC[i][3], "R+", false);
        draw_btn(dc, G_SONG_RC[i][4], "上移", false);
        draw_btn(dc, G_SONG_RC[i][5], "下移", false);
        draw_btn(dc, G_SONG_RC[i][6], "删", false);
    }

    // 右侧翻页按钮
    G_SONG_UP = RECT::new(w - 62, SG_LIST_Y, w - 16, SG_LIST_Y + 26);
    G_SONG_DN = RECT::new(w - 62, SG_LIST_Y + list_h - 26, w - 16, SG_LIST_Y + list_h);
    draw_btn(dc, G_SONG_UP, "▲", false);
    draw_btn(dc, G_SONG_DN, "▼", false);

    // 底部操作
    let by = SG_LIST_Y + list_h + 10;
    G_SONG_ADD = RECT::new(12, by, 132, by + 28);
    G_SONG_CLR = RECT::new(140, by, 202, by + 28);
    G_SONG_CLOSE = RECT::new(w - 108, by, w - 12, by + 28);
    draw_btn(dc, G_SONG_ADD, "追加当前", true);
    draw_btn(dc, G_SONG_CLR, "清空", false);
    draw_btn(dc, G_SONG_CLOSE, "关闭", false);

    text_at(
        dc,
        12,
        by + 34,
        w - 24,
        14,
        &format!("共 {} 条 · SONG 模式打开时按此顺序循环播放", n),
        C_DIM,
        G_FONT_TINY,
        DT_LEFT,
    );
}

/// 歌曲链窗口的点击分发
unsafe fn song_click(x: i32, y: i32) {
    let n = eng().proj.song.len();
    let top = if n > SG_ROWS { G_SONG_TOP.min(n - SG_ROWS) } else { 0 };

    // 1) 行内控件
    for i in 0..SG_ROWS {
        let idx = top + i;
        for b in 0..SG_BTNS {
            if idx < n && G_SONG_RC[i][b].inside(x, y) {
                G_SONG_SEL = idx;
                match b {
                    0 => {
                        let e = &mut eng().proj.song[idx];
                        e.pattern = (e.pattern - 1 + PATTERNS as i32) % PATTERNS as i32;
                    }
                    1 => {
                        let e = &mut eng().proj.song[idx];
                        e.pattern = (e.pattern + 1) % PATTERNS as i32;
                    }
                    2 => {
                        let e = &mut eng().proj.song[idx];
                        e.repeats = (e.repeats - 1).max(1);
                    }
                    3 => {
                        let e = &mut eng().proj.song[idx];
                        e.repeats = (e.repeats + 1).min(64);
                    }
                    4 => {
                        if idx > 0 {
                            eng().proj.song.swap(idx, idx - 1);
                            G_SONG_SEL = idx - 1;
                        }
                    }
                    5 => {
                        if idx + 1 < n {
                            eng().proj.song.swap(idx, idx + 1);
                            G_SONG_SEL = idx + 1;
                        }
                    }
                    _ => {
                        eng().proj.song.remove(idx);
                        song_clamp();
                    }
                }
                InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
                return;
            }
        }
    }

    // 2) 选行（点空白处也算选中该行）
    let list_h = SG_ROWS as i32 * SG_ROW_H;
    if y >= SG_LIST_Y && y < SG_LIST_Y + list_h {
        let i = ((y - SG_LIST_Y) / SG_ROW_H) as usize;
        let idx = top + i;
        if idx < n {
            G_SONG_SEL = idx;
            InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        }
        return;
    }

    // 3) 翻页
    if G_SONG_UP.inside(x, y) {
        G_SONG_TOP = G_SONG_TOP.saturating_sub(1);
        InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        return;
    }
    if G_SONG_DN.inside(x, y) {
        if n > SG_ROWS && G_SONG_TOP + SG_ROWS < n {
            G_SONG_TOP += 1;
        }
        InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        return;
    }

    // 4) 追加 / 清空 / 关闭
    if G_SONG_ADD.inside(x, y) {
        if n < 64 {
            let pat = eng().proj.current;
            eng().proj.song.push(crate::project::SongEntry { pattern: pat, repeats: 1 });
            let nn = eng().proj.song.len();
            G_SONG_SEL = nn - 1;
            if nn > SG_ROWS {
                G_SONG_TOP = nn - SG_ROWS;
            }
            InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        }
        return;
    }
    if G_SONG_CLR.inside(x, y) {
        eng().proj.song.clear();
        G_SONG_SEL = 0;
        G_SONG_TOP = 0;
        InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        return;
    }
    if G_SONG_CLOSE.inside(x, y) {
        DestroyWindow(G_SONG_HWND);
    }
}

/// 打开（或前置）歌曲链窗口
unsafe fn open_song_editor() {
    if !G_SONG_HWND.is_null() {
        InvalidateRect(G_SONG_HWND, std::ptr::null(), 0);
        return;
    }
    song_clamp();

    // 居中于主窗口
    let mut owner = RECT::default();
    GetWindowRect(G_HWND, &mut owner);
    let style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    let mut rc = RECT::new(0, 0, SG_W, SG_H);
    AdjustWindowRect(&mut rc, style, 0);
    let ww = rc.right - rc.left;
    let hh = rc.bottom - rc.top;
    let cx = (owner.left + owner.right) / 2 - ww / 2;
    let cy = (owner.top + owner.bottom) / 2 - hh / 2;

    let cls = wide("XU808_SONGDLG");
    let title = wide("歌曲链编辑 · 徐某");
    let inst = GetModuleHandleW(std::ptr::null());
    let hwnd = CreateWindowExW(
        0,
        cls.as_ptr(),
        title.as_ptr(),
        style,
        cx,
        cy,
        ww,
        hh,
        G_HWND,
        std::ptr::null_mut(),
        inst,
        std::ptr::null_mut(),
    );
    if hwnd.is_null() {
        return;
    }
    G_SONG_HWND = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

/// 歌曲链窗口过程
unsafe extern "system" fn song_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_PAINT => {
            let mut ps = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut ps);
            let mut rc = RECT::default();
            GetClientRect(hwnd, &mut rc);
            // 同样双缓冲，避免列表刷新时闪烁
            let mem = CreateCompatibleDC(dc);
            let bmp = CreateCompatibleBitmap(dc, rc.right.max(1), rc.bottom.max(1));
            let old = SelectObject(mem, bmp as HGDIOBJ);
            song_paint(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp as HGDIOBJ);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            0
        }
        WM_LBUTTONDOWN => {
            song_click(loword_signed(lp), hiword_signed(lp));
            0
        }
        WM_CLOSE => {
            DestroyWindow(hwnd);
            0
        }
        WM_DESTROY => {
            if G_SONG_HWND == hwnd {
                G_SONG_HWND = std::ptr::null_mut();
            }
            0
        }
        _ => DefWindowProcW(hwnd, msg, wp, lp),
    }
}

// ============================================================================
//  窗口过程
// ============================================================================
unsafe extern "system" fn wnd_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_CREATE => {
            G_HWND = hwnd;
            SetTimer(hwnd, 1, 33, std::ptr::null_mut()); // 30fps，只驱动 UI
            0
        }
        WM_SIZE => {
            layout_grid(hwnd);
            0
        }
        WM_TIMER => {
            midi::poll(eng());
            InvalidateRect(hwnd, std::ptr::null(), 0);
            0
        }
        WM_PAINT => {
            let mut ps = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut ps);
            let mut rc = RECT::default();
            GetClientRect(hwnd, &mut rc);
            // 双缓冲：先画到内存位图，再一次性 BitBlt，彻底消除闪烁
            let mem = CreateCompatibleDC(dc);
            let bmp = CreateCompatibleBitmap(dc, rc.right.max(1), rc.bottom.max(1));
            let old = SelectObject(mem, bmp as HGDIOBJ);
            paint(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp as HGDIOBJ);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            0
        }
        WM_LBUTTONDOWN => {
            on_lbutton(loword_signed(lp), hiword_signed(lp));
            0
        }
        WM_RBUTTONDOWN => {
            on_rbutton(loword_signed(lp), hiword_signed(lp));
            0
        }
        WM_MOUSEMOVE => {
            on_mouse_move(loword_signed(lp), hiword_signed(lp));
            0
        }
        WM_LBUTTONUP => {
            if G_DRAG_PARAM >= 0 || G_DRAG_MASTER >= 0 {
                G_DRAG_PARAM = -1;
                G_DRAG_MASTER = -1;
                ReleaseCapture();
            }
            0
        }
        WM_KEYDOWN => {
            let vk = wp;
            if vk == VK_SPACE {
                if eng().playing {
                    eng().stop();
                } else {
                    eng().play();
                }
                return 0;
            }
            // F2：打开歌曲链编辑窗口（与顶栏「歌曲链」按钮等价）
            if vk == 0x71 {
                open_song_editor();
                return 0;
            }
            // 数字 1..8 切 Pattern
            if (0x31..=0x38).contains(&vk) {
                let n = (vk - 0x31) as i32;
                eng().request_pattern(n);
                layout_grid(hwnd);
                return 0;
            }
            // 16 键鼓垫：Z X C V B N M A S D F G H J K L
            let keys = "ZXCVBNMASDFGHJKL";
            let ch = char::from_u32(vk as u32).unwrap_or('\0').to_ascii_uppercase();
            if let Some(pos) = keys.find(ch) {
                ui_trigger_pad(pos as i32, 110, false);
            }
            0
        }
        WM_CLOSE => {
            KillTimer(hwnd, 1);
            eng().stop();
            audio::stop();
            midi::stop();
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(hwnd, msg, wp, lp),
    }
}

// ============================================================================
//  入口：建窗口、起音频/MIDI、跑消息循环
// ============================================================================
pub fn run() -> i32 {
    unsafe {
        // 先声明 DPI 感知：否则高分屏上 GDI 坐标会被系统缩放，
        // 1500 宽的窗口会被虚拟化到屏幕外，右侧按钮就看不见了。
        SetProcessDPIAware();

        let inst = GetModuleHandleW(std::ptr::null());
        let title = wide("XU-808 鼓机 - 徐某 (Rust 版)");
        let cls_name = wide("XU808_RUST_WINDOW");

        // 引擎必须在建窗口之前就绪：CreateWindowExW 期间会收到 WM_SIZE，
        // 那时 layout_grid 要读工程数据，晚一步就会读到空指针。
        G_ENG = Box::into_raw(Box::new(Engine::new(Project::default_project())));

        // 字体一律用系统自带的，绝不打包字体文件
        G_FONT_TINY = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Microsoft YaHei UI").as_ptr());
        G_FONT_SMALL = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Microsoft YaHei UI").as_ptr());
        G_FONT_NORM = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Microsoft YaHei UI").as_ptr());
        G_FONT_BOLD = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Microsoft YaHei UI").as_ptr());
        G_FONT_MONO = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Consolas").as_ptr());
        G_FONT_LOGO = CreateFontW(-34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, wide("Consolas").as_ptr());

        let wc = WNDCLASSW {
            style: 0,
            lpfnWndProc: Some(wnd_proc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: inst,
            hIcon: LoadIconW(inst, make_int_resource(1)), // 资源里 ID=1 的 app.ico
            hCursor: LoadCursorW(std::ptr::null_mut(), make_int_resource(IDC_ARROW)),
            hbrBackground: stock_object(BLACK_BRUSH),
            lpszMenuName: std::ptr::null(),
            lpszClassName: cls_name.as_ptr(),
        };
        RegisterClassW(&wc);

        // 歌曲链编辑弹窗单独一个窗口类（非模态，可和主窗口同时操作）
        let song_cls = wide("XU808_SONGDLG");
        let wc_song = WNDCLASSW {
            style: 0,
            lpfnWndProc: Some(song_proc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: inst,
            hIcon: LoadIconW(inst, make_int_resource(1)),
            hCursor: LoadCursorW(std::ptr::null_mut(), make_int_resource(IDC_ARROW)),
            hbrBackground: stock_object(BLACK_BRUSH),
            lpszMenuName: std::ptr::null(),
            lpszClassName: song_cls.as_ptr(),
        };
        RegisterClassW(&wc_song);

        // 客户区默认 1500×900（所有面板都按这个宽度排版）。
        // 屏幕放不下时按“屏幕 − 任务栏 − 边框”收缩，保证右上导出按钮和
        // 底部 MASTER 电平表都留在可见区内；内容最矮需要 742px。
        let style = WS_OVERLAPPEDWINDOW & !WS_MAXIMIZEBOX;
        let mut frame = RECT::new(0, 0, 0, 0);
        AdjustWindowRect(&mut frame, style, 0); // 量出标题栏 + 边框占的像素
        let frame_w = frame.right - frame.left;
        let frame_h = frame.bottom - frame.top;
        let screen_w = GetSystemMetrics(0); // SM_CXSCREEN
        let screen_h = GetSystemMetrics(1); // SM_CYSCREEN
        let client_w = 1500.min((screen_w - frame_w).max(1000));
        let client_h = 900.min((screen_h - 48 - frame_h).max(700)); // 48px 预留给任务栏
        let mut rc = RECT::new(0, 0, client_w, client_h);
        AdjustWindowRect(&mut rc, style, 0);
        let hwnd = CreateWindowExW(
            0,
            cls_name.as_ptr(),
            title.as_ptr(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rc.right - rc.left,
            rc.bottom - rc.top,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            inst,
            std::ptr::null_mut(),
        );
        if hwnd.is_null() {
            return 1;
        }

        // 引擎放堆上（voice 噪声池约 2MB），并用裸指针共享给音频回调
        layout_top();
        layout_grid(hwnd);

        // 先把窗口亮出来，再做音频初始化。
        // waveOutOpen 在无声卡 / 远程桌面 / 音频服务忙时可能阻塞数秒，
        // 放在 UI 线程里会把窗口拖成“未响应”，必须在 ShowWindow 之后。
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        // 音频与 MIDI 初始化丢到后台线程：引擎指针以整数形式传进去，
        // 线程里再转回裸指针（引擎在消息循环期间一直存活）。
        let eng_addr = G_ENG as usize;
        std::thread::spawn(move || {
            audio::start(eng_addr as *mut Engine);
            midi::start();
        });

        let mut msg = MSG::default();
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 收尾：确保音频线程已停，再释放引擎
        audio::stop();
        midi::stop();
        if !G_ENG.is_null() {
            drop(Box::from_raw(G_ENG));
            G_ENG = std::ptr::null_mut();
        }
        0
    }
}
