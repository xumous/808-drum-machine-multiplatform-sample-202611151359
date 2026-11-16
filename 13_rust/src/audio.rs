// ============================================================================
//  audio.rs —— waveOut 四缓冲回调输出（作者：徐某）
//
//  Win32 的 waveOut 是“回调 + 缓冲队列”模型：
//    · 打开设备时给一个 CALLBACK_FUNCTION，系统在每次缓冲播完时回调我们；
//    · 我们准备 4 个块，连成一条输送带；回调里渲染下一块再 waveOutWrite；
//    · 只要回调里不做任何可能阻塞的事，就永远不断音。
//
//  这是纯 FFI 版本，没有 audio crate，也没有 COM，离线也能编。
// ============================================================================
use std::sync::atomic::{AtomicI32, Ordering};

use crate::engine::{Engine, SHARED};
use crate::project::BLOCK;
use crate::win::*;

const NBUFS: usize = 4; // 四缓冲：256*4 = 1024 样本 ≈ 21ms 安全余量

struct Audio {
    h: HWAVEOUT,
    hdr: [WAVEHDR; NBUFS],
    data: Box<[[i16; BLOCK * 2]; NBUFS]>,
    next: usize,
    running: bool,
    eng: *mut Engine,
}

static mut AUDIO: *mut Audio = std::ptr::null_mut();
/// 正在回调里的线程数，stop 时要等它归零再释放内存（防 use-after-free）
static CB_BUSY: AtomicI32 = AtomicI32::new(0);
/// 初始化状态：0=未开始 1=正在打开设备 2=就绪 3=打开失败
/// waveOutOpen 在无声卡 / 远程桌面下可能阻塞几秒，UI 靠这个状态判断“别乱动内存”
static INIT_STATE: AtomicI32 = AtomicI32::new(0);

/// 供 UI 查询：0 未开始 / 1 打开中 / 2 就绪 / 3 失败
pub fn init_state() -> i32 {
    INIT_STATE.load(Ordering::Relaxed)
}

/// waveOut 回调：每块播完触发一次，在这里完成“渲染 → 转 int16 → 再排队”
unsafe extern "system" fn wave_cb(
    _h: HWAVEOUT,
    msg: UINT,
    _inst: DWORD_PTR,
    _p1: DWORD_PTR,
    _p2: DWORD_PTR,
) {
    if msg != WOM_DONE {
        return;
    }
    CB_BUSY.fetch_add(1, Ordering::SeqCst);
    render_one_block();
    CB_BUSY.fetch_sub(1, Ordering::SeqCst);
}

unsafe fn render_one_block() {
    let ap = std::ptr::addr_of!(AUDIO).read();
    if ap.is_null() {
        return;
    }
    let a = &mut *ap;
    if !a.running {
        return;
    }
    let started = std::time::Instant::now();

    // ---- 渲染一块（唯一的声音出口，与离线导出走同一段代码） ----
    let e = &mut *a.eng;
    e.render(BLOCK);

    let idx = a.next;
    let buf = &mut a.data[idx];
    for i in 0..BLOCK {
        let mut l = e.l[i];
        let mut r = e.r[i];
        if l < -1.0 {
            l = -1.0;
        }
        if l > 1.0 {
            l = 1.0;
        }
        if r < -1.0 {
            r = -1.0;
        }
        if r > 1.0 {
            r = 1.0;
        }
        buf[i * 2] = (l * 32767.0) as i16;
        buf[i * 2 + 1] = (r * 32767.0) as i16;
    }
    a.next = (idx + 1) & (NBUFS - 1);
    let h = a.h;
    waveOutWrite(h, &mut a.hdr[idx], std::mem::size_of::<WAVEHDR>() as u32);

    // ---- CPU 占用估计（渲染耗时 / 本块应有的 5.33ms） ----
    let dt = started.elapsed().as_secs_f64();
    let ideal = BLOCK as f64 / crate::project::SR as f64;
    let old = SHARED.cpu();
    let cpu = old * 0.95 + (dt / ideal) as f32 * 0.05;
    SHARED.cpu.store(cpu.to_bits(), Ordering::Relaxed);
    if dt > ideal {
        SHARED.xruns.fetch_add(1, Ordering::Relaxed);
    }
}

/// 启动音频设备；成功返回 true
pub unsafe fn start(eng: *mut Engine) -> bool {
    if !std::ptr::addr_of!(AUDIO).read().is_null() {
        return true;
    }
    INIT_STATE.store(1, Ordering::Relaxed); // 标记“正在打开设备”
    let data: Box<[[i16; BLOCK * 2]; NBUFS]> = Box::new([[0i16; BLOCK * 2]; NBUFS]);
    let audio = Box::new(Audio {
        h: std::ptr::null_mut(),
        hdr: [WAVEHDR::default(); NBUFS],
        data,
        next: 0,
        running: false,
        eng,
    });
    let ap = Box::into_raw(audio);
    AUDIO = ap;
    let a = &mut *ap;

    // 44.1/48k 一律 48k、16bit、立体声（规格第 1 节）
    let fmt = WAVEFORMATEX {
        wFormatTag: WAVE_FORMAT_PCM,
        nChannels: 2,
        nSamplesPerSec: crate::project::SR as u32,
        nAvgBytesPerSec: (crate::project::SR * 4) as u32,
        nBlockAlign: 4,
        wBitsPerSample: 16,
        cbSize: 0,
    };
    let mut h: HWAVEOUT = std::ptr::null_mut();
    let rc = waveOutOpen(
        &mut h,
        WAVE_MAPPER,
        &fmt,
        wave_cb as *const () as usize,
        0,
        CALLBACK_FUNCTION,
    );
    if rc != MMSYSERR_NOERROR {
        // 没有声卡也别崩：把内存收回，返回 false，UI 会显示“无音频”
        drop(Box::from_raw(ap));
        AUDIO = std::ptr::null_mut();
        INIT_STATE.store(3, Ordering::Relaxed);
        return false;
    }
    a.h = h;

    for i in 0..NBUFS {
        let dp = a.data[i].as_mut_ptr() as *mut u8;
        a.hdr[i] = WAVEHDR {
            lpData: dp,
            dwBufferLength: (BLOCK * 4) as u32,
            ..WAVEHDR::default()
        };
        waveOutPrepareHeader(a.h, &mut a.hdr[i], std::mem::size_of::<WAVEHDR>() as u32);
    }

    // 先填满 4 块再开闸 —— 否则开头会有半块静音
    a.running = true;
    for i in 0..NBUFS {
        let e = &mut *a.eng;
        e.render(BLOCK);
        for k in 0..BLOCK {
            let mut l = e.l[k];
            let mut r = e.r[k];
            if l < -1.0 {
                l = -1.0;
            }
            if l > 1.0 {
                l = 1.0;
            }
            if r < -1.0 {
                r = -1.0;
            }
            if r > 1.0 {
                r = 1.0;
            }
            a.data[i][k * 2] = (l * 32767.0) as i16;
            a.data[i][k * 2 + 1] = (r * 32767.0) as i16;
        }
        waveOutWrite(a.h, &mut a.hdr[i], std::mem::size_of::<WAVEHDR>() as u32);
    }
    a.next = 0;
    SHARED.audio_ok.store(true, Ordering::Relaxed);
    INIT_STATE.store(2, Ordering::Relaxed); // 设备就绪
    true
}

/// 停止并释放（可重复调用）
pub unsafe fn stop() {
    let ap = std::ptr::addr_of!(AUDIO).read();
    if ap.is_null() {
        return;
    }
    // 后台线程还在 waveOutOpen 里阻塞：此时内存结构未完整，什么都不碰，
    // 交给进程退出时由系统回收（否则就是 use-after-free）。
    if INIT_STATE.load(Ordering::Relaxed) == 1 {
        return;
    }
    let a = &mut *ap;
    a.running = false;
    waveOutReset(a.h);
    // 等所有在途回调退出，绝不带着野指针释放
    for _ in 0..500 {
        if CB_BUSY.load(Ordering::SeqCst) == 0 {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    for i in 0..NBUFS {
        waveOutUnprepareHeader(a.h, &mut a.hdr[i], std::mem::size_of::<WAVEHDR>() as u32);
    }
    waveOutClose(a.h);
    drop(Box::from_raw(ap));
    AUDIO = std::ptr::null_mut();
    SHARED.audio_ok.store(false, Ordering::Relaxed);
    INIT_STATE.store(0, Ordering::Relaxed); // 允许下次重新 start
}
