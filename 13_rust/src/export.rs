// ============================================================================
//  export.rs —— WAV / MIDI 离线导出（作者：徐某）
//
//  WAV 导出刻意复用 Engine::render —— 和实时输出走的是同一段 DSP/音序代码，
//  只是把时钟换成“固定块推进”，所以导出结果与实时听感完全一致。
//
//  MIDI 导出写成 SMF Format 1、PPQ=480（16 分音符 = 120 tick），
//  微时换算成 tick、ratchet 展开成多个音符，DAW 可正确导入。
// ============================================================================
use std::fs::File;
use std::io::{self, Write};

use crate::engine::Engine;
use crate::project::{Project, MIDI_NOTES, PADS, PATTERNS, SR};

/// 计算整曲/单 pattern 的总步数
fn total_steps(p: &Project, song: bool) -> i64 {
    let mut n: i64 = 0;
    if song && !p.song.is_empty() {
        for e in p.song.iter() {
            let idx = e.pattern.clamp(0, PATTERNS as i32 - 1) as usize;
            let rep = e.repeats.max(1);
            n += p.patterns[idx].steps as i64 * rep as i64;
        }
    } else {
        n = p.patterns[p.cur()].steps as i64;
    }
    if n <= 0 {
        16
    } else {
        n
    }
}

fn put_u32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}
fn put_u16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_le_bytes());
}

/// 导出 16bit / 48kHz / 立体声 WAV
pub fn export_wav(p: &Project, path: &str, song: bool, solo_pad: i32) -> io::Result<u64> {
    let mut eng = Engine::new(p.clone());
    eng.solo_override = solo_pad;
    eng.play();

    let steps = total_steps(p, song);
    let sl = eng.step_len() as i64;
    let frames = steps * sl + 3 * SR as i64; // 末尾留 3 秒尾音
    let data_bytes = frames * 4;

    let mut f = File::create(path)?;

    // ---- RIFF 头（44 字节，标准 PCM） ----
    let mut hdr: Vec<u8> = Vec::with_capacity(44);
    hdr.extend_from_slice(b"RIFF");
    put_u32(&mut hdr, (36 + data_bytes) as u32);
    hdr.extend_from_slice(b"WAVEfmt ");
    put_u32(&mut hdr, 16); // fmt chunk 长度
    put_u16(&mut hdr, 1); // PCM
    put_u16(&mut hdr, 2); // 双声道
    put_u32(&mut hdr, SR as u32);
    put_u32(&mut hdr, (SR * 4) as u32);
    put_u16(&mut hdr, 4); // block align
    put_u16(&mut hdr, 16); // 位深
    hdr.extend_from_slice(b"data");
    put_u32(&mut hdr, data_bytes as u32);
    f.write_all(&hdr)?;

    // ---- 逐块渲染 ----
    let mut out = vec![0u8; 256 * 4];
    let mut done: i64 = 0;
    while done < frames {
        let n = ((frames - done) as usize).min(256);
        eng.render(n);
        for i in 0..n {
            let mut l = eng.l[i];
            let mut r = eng.r[i];
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
            let a = (l * 32767.0) as i16;
            let b = (r * 32767.0) as i16;
            out[i * 4] = (a & 0xff) as u8;
            out[i * 4 + 1] = (a >> 8) as u8;
            out[i * 4 + 2] = (b & 0xff) as u8;
            out[i * 4 + 3] = (b >> 8) as u8;
        }
        f.write_all(&out[..n * 4])?;
        done += n as i64;
    }
    f.flush()?;
    Ok((44 + data_bytes) as u64)
}

// ============================================================================
//  MIDI
// ============================================================================
#[derive(Clone, Copy)]
struct Note {
    tick: i32,
    note: i32,
    vel: i32,
}

/// VLQ（可变长数量）编码：MIDI 的标准 delta-time 表示
fn vlq(out: &mut Vec<u8>, value: i32) {
    let mut v = if value < 0 { 0 } else { value };
    let mut tmp = [0u8; 5];
    let mut n = 0usize;
    tmp[n] = (v & 0x7f) as u8;
    n += 1;
    v >>= 7;
    while v > 0 {
        tmp[n] = ((v & 0x7f) | 0x80) as u8;
        n += 1;
        v >>= 7;
    }
    for i in (0..n).rev() {
        out.push(tmp[i]);
    }
}

/// 导出 SMF Format 1（PPQ=480）
pub fn export_midi(p: &Project, path: &str, song: bool) -> io::Result<()> {
    let ppq: u16 = 480;
    let bpm = p.master.bpm.clamp(40.0, 300.0) as f64;
    let step_samples = SR as f64 * 60.0 / bpm / 4.0;

    // 先把要导出的 pattern 序列展开（song 模式按 repeats 复制）
    let mut seq: Vec<usize> = Vec::new();
    if song && !p.song.is_empty() {
        for e in p.song.iter() {
            let idx = e.pattern.clamp(0, PATTERNS as i32 - 1) as usize;
            let rep = e.repeats.max(1);
            for _ in 0..rep {
                if seq.len() >= 256 {
                    break;
                }
                seq.push(idx);
            }
        }
    } else {
        seq.push(p.cur());
    }

    let mut notes: Vec<Note> = Vec::with_capacity(8192);
    let mut cursor: i64 = 0; // 已经走过的总步数（polymeter 要按它取模）

    for &pi in seq.iter() {
        let pat = &p.patterns[pi];
        for s in 0..pat.steps {
            // Swing 换算成 tick：75% 摇摆时奇数步偏移 45 tick（=0.75*60）
            let swing_tick = if s % 2 != 0 { (p.master.swing * 60.0) as i32 } else { 0 };
            for pad in 0..PADS {
                let st = *pat.step(pad, cursor + s as i64);
                if !st.on || st.prob == 0 {
                    continue;
                }
                let mut vel = (st.velocity as f32 * if st.accent { 1.3 } else { 1.0 }) as i32;
                vel = vel.clamp(1, 127);
                let base_tick = ((cursor + s as i64) * 120) as i32;
                let micro_tick = (st.micro as f64 / step_samples * 120.0) as i32;
                let r = st.ratchet.clamp(1, 8) as i32;
                for i in 0..r {
                    let mut tick = base_tick + swing_tick + micro_tick + (i * 120 / r);
                    if tick < 0 {
                        tick = 0;
                    }
                    notes.push(Note { tick, note: MIDI_NOTES[pad], vel });
                }
            }
        }
        cursor += pat.steps as i64;
    }
    notes.sort_by(|a, b| a.tick.cmp(&b.tick).then(a.note.cmp(&b.note)));

    // ---- 构造唯一一条轨道 ----
    let mut track: Vec<u8> = Vec::with_capacity(65536);
    let name = "XU-808 by XuMou";
    vlq(&mut track, 0);
    track.push(0xFF);
    track.push(0x03); // track name
    vlq(&mut track, name.len() as i32);
    track.extend_from_slice(name.as_bytes());

    // 速度
    let tempo = (60_000_000.0 / bpm) as i32;
    vlq(&mut track, 0);
    track.push(0xFF);
    track.push(0x51);
    track.push(0x03);
    track.push((tempo >> 16) as u8);
    track.push((tempo >> 8) as u8);
    track.push(tempo as u8);

    // 拍号
    vlq(&mut track, 0);
    track.push(0xFF);
    track.push(0x58);
    track.push(0x04);
    track.push(p.master.beats_per_bar.clamp(1, 16) as u8);
    let mut bu = p.master.beat_unit;
    let mut du = 0u8;
    while bu > 1 {
        bu >>= 1;
        du += 1;
    }
    track.push(du);
    track.push(24);
    track.push(8);

    // 音符：每个音 10 tick 长（鼓机单击）
    let mut last = 0i32;
    for nt in notes.iter() {
        vlq(&mut track, nt.tick - last);
        track.push(0x99); // Note On, channel 10（0 基的 9）
        track.push(nt.note as u8);
        track.push(nt.vel as u8);
        vlq(&mut track, 10);
        track.push(0x89);
        track.push(nt.note as u8);
        track.push(0x40);
        last = nt.tick + 10;
    }
    vlq(&mut track, 0);
    track.push(0xFF);
    track.push(0x2F); // End of Track
    track.push(0x00);

    // ---- 写文件 ----
    let mut f = File::create(path)?;
    f.write_all(b"MThd")?;
    f.write_all(&6u32.to_be_bytes())?;
    f.write_all(&1u16.to_be_bytes())?; // format 1
    f.write_all(&1u16.to_be_bytes())?; // 1 track
    f.write_all(&ppq.to_be_bytes())?;
    f.write_all(b"MTrk")?;
    f.write_all(&(track.len() as u32).to_be_bytes())?;
    f.write_all(&track)?;
    f.flush()?;
    Ok(())
}
