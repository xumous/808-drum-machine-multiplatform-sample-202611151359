// ============================================================================
//  main.rs —— XU-808 Rust 版入口（作者：徐某）
//
//  两种运行方式：
//    1. 正常启动      → 打开 Win32 窗口，waveOut 播放；
//    2. --selftest    → 无界面离线自检：导出 WAV/MIDI + JSON 往返，
//                      并写 selftest_rust.txt（内容含 SELFTEST OK），
//                      方便构建脚本在无人值守的情况下判断成功与否。
//
//  release 版使用 windows 子系统（没有黑框），debug 版保留控制台方便排错。
// ============================================================================
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
#![allow(static_mut_refs)]

mod audio;
mod dsp;
mod engine;
mod export;
mod midi;
mod project;
mod ui;
mod win;

use std::fs;
use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a.contains("--selftest")) {
        let ok = run_selftest();
        std::process::exit(if ok { 0 } else { 1 });
    }
    ui::run();
}

/// 无界面自检：完整走一遍 DSP/音序/导出/存档链路
fn run_selftest() -> bool {
    let proj = project::Project::default_project();

    // ---- 1. 导出 WAV（单 pattern） ----
    let wav = "selftest_rust.wav";
    let wav_ok = match export::export_wav(&proj, wav, false, -1) {
        Ok(bytes) => bytes > 44,
        Err(_) => false,
    };
    let (wav_bytes, peak, rms) = match fs::read(wav) {
        Ok(d) => {
            let mut peak = 0i32;
            let mut sumsq: f64 = 0.0;
            let mut cnt: u64 = 0;
            let mut i = 44;
            while i + 1 < d.len() {
                let s = i16::from_le_bytes([d[i], d[i + 1]]) as i32;
                if s.abs() > peak {
                    peak = s.abs();
                }
                sumsq += (s as f64) * (s as f64);
                cnt += 1;
                i += 2;
            }
            let rms = if cnt > 0 { (sumsq / cnt as f64).sqrt() } else { 0.0 };
            (d.len(), peak, rms)
        }
        Err(_) => (0, 0, 0.0),
    };

    // ---- 2. 导出 MIDI ----
    let midi_ok = export::export_midi(&proj, "selftest_rust.mid", false).is_ok();
    let midi_bytes = fs::metadata("selftest_rust.mid").map(|m| m.len()).unwrap_or(0);

    // ---- 3. 工程 JSON 往返 ----
    let json_ok = proj.save_json("selftest_rust.xu808").is_ok();
    let roundtrip_ok = match project::Project::load_json("selftest_rust.xu808") {
        Ok(p2) => {
            p2.patterns.len() == proj.patterns.len()
                && p2.patterns[0].steps == proj.patterns[0].steps
                && p2.patterns[0].data[0][0].on == proj.patterns[0].data[0][0].on
                && (p2.master.bpm - proj.master.bpm).abs() < 0.01
        }
        Err(_) => false,
    };

    let ok = wav_ok && wav_bytes > 44 && peak > 1000 && midi_ok && midi_bytes > 44 && json_ok && roundtrip_ok;

    // ---- 4. 写标记文件 ----
    let mut msg = String::new();
    msg.push_str(if ok { "SELFTEST OK\r\n" } else { "SELFTEST FAIL\r\n" });
    msg.push_str(&format!(
        "wav={} bytes={} peak={} rms={:.1}\r\nmidi={} bytes={}\r\njson={} roundtrip={}\r\n",
        wav_ok, wav_bytes, peak, rms, midi_ok, midi_bytes, json_ok, roundtrip_ok
    ));
    msg.push_str("XU-808 Rust edition by 徐某\r\n");
    if let Ok(mut f) = fs::File::create("selftest_rust.txt") {
        let _ = f.write_all(msg.as_bytes());
    }
    ok
}
