// ============================================================================
//  midi.rs —— MIDI 输入线程（作者：徐某）
//
//  结构：winmm 的 midiIn 回调（系统线程）只做一件事——把一个 3 字节消息
//  压进无锁环形队列；UI 定时器每 33ms 消费一次，转成引擎动作。
//
//  这样音频线程永远不会被 MIDI 阻塞，MIDI 线程也永远不碰引擎状态。
// ============================================================================
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

use crate::engine::{ui_trigger_pad, Engine, SHARED};
use crate::project::{MIDI_NOTES, PADS, PATTERNS};
use crate::win::*;

const MCAP: usize = 256;

// 每个事件打包成 u32：status | d1<<8 | d2<<16
static M_BUF: [AtomicU32; MCAP] = [const { AtomicU32::new(0) }; MCAP];
static M_HEAD: AtomicUsize = AtomicUsize::new(0);
static M_TAIL: AtomicUsize = AtomicUsize::new(0);

static mut MIDI_HANDLE: HMIDIIN = std::ptr::null_mut();

unsafe extern "system" fn midi_cb(
    _h: HMIDIIN,
    msg: UINT,
    _inst: DWORD_PTR,
    p1: DWORD_PTR,
    _p2: DWORD_PTR,
) {
    if msg != MIM_DATA {
        return;
    }
    let seat = M_TAIL.load(Ordering::Acquire);
    let head = M_HEAD.load(Ordering::Acquire);
    if seat.wrapping_sub(head) >= MCAP {
        return; // 满则丢，MIDI 线程绝不阻塞
    }
    let st = (p1 & 0xff) as u32;
    let d1 = ((p1 >> 8) & 0xff) as u32;
    let d2 = ((p1 >> 16) & 0xff) as u32;
    M_BUF[seat % MCAP].store(st | (d1 << 8) | (d2 << 16), Ordering::Release);
    M_TAIL.store(seat.wrapping_add(1), Ordering::Release);
}

/// 打开系统中第一个 MIDI 输入设备；没有设备就安静地什么都不做
pub unsafe fn start() {
    if !std::ptr::addr_of!(MIDI_HANDLE).read().is_null() {
        return;
    }
    if midiInGetNumDevs() == 0 {
        return;
    }
    let mut h: HMIDIIN = std::ptr::null_mut();
    let rc = midiInOpen(&mut h, 0, midi_cb as *const () as usize, 0, CALLBACK_FUNCTION);
    if rc == MMSYSERR_NOERROR {
        MIDI_HANDLE = h;
        midiInStart(h);
        SHARED.midi_on.store(true, Ordering::Relaxed);
    }
}

pub unsafe fn stop() {
    let h = std::ptr::addr_of!(MIDI_HANDLE).read();
    if h.is_null() {
        return;
    }
    midiInStop(h);
    midiInReset(h);
    midiInClose(h);
    MIDI_HANDLE = std::ptr::null_mut();
    SHARED.midi_on.store(false, Ordering::Relaxed);
}

/// UI 定时器调用：消费队列里的 MIDI 事件
pub fn poll(e: &mut Engine) {
    let mut head = M_HEAD.load(Ordering::Acquire);
    let tail = M_TAIL.load(Ordering::Acquire);
    while head != tail {
        let d = M_BUF[head % MCAP].load(Ordering::Acquire);
        head = head.wrapping_add(1);
        let st = (d & 0xff) as u8;
        let d1 = ((d >> 8) & 0xff) as u8;
        let d2 = ((d >> 16) & 0xff) as u8;
        handle(e, st, d1, d2);
    }
    M_HEAD.store(head, Ordering::Release);
}

fn handle(e: &mut Engine, st: u8, d1: u8, d2: u8) {
    let cmd = st & 0xF0;

    // Note On（velocity>0）：按音高映射触发声部；Note Off 不处理（鼓机单击即响）
    if cmd == 0x90 && d2 > 0 {
        for pad in 0..PADS {
            if MIDI_NOTES[pad] == d1 as i32 {
                ui_trigger_pad(pad as i32, d2 as i32, d2 >= 120);
                break;
            }
        }
    } else if cmd == 0xB0 {
        // CC 7 = 主音量，CC 74 = 低通截止，CC 71 = 共鸣
        match d1 {
            7 => e.proj.master.volume = d2 as f32 / 127.0,
            74 => e.proj.master.lp_cutoff = (20.0 * 1000f64.powf(d2 as f64 / 127.0)) as f32,
            71 => e.proj.master.lp_q = 0.5 + d2 as f32 / 127.0 * 7.5,
            _ => {}
        }
    } else if cmd == 0xC0 {
        // Program Change 是“单数据字节”消息：程序号在 d1 里，d2 无意义（恒为 0）
        e.request_pattern((d1 as i32) % PATTERNS as i32);
    } else if st == 0xFA {
        e.play();
    } else if st == 0xFC {
        e.stop();
    } else if st == 0xF8 && e.clock_external {
        e.external_tick();
    }
}
