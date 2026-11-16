// ==========================================================================
//  export.go —— 离线渲染：WAV（整曲/单 Pattern/分轨）与 MIDI（SMF Format 1）
//
//  署名：徐某。
//  关键点：离线导出绝不另写一套 DSP，而是新建一个 Engine，用与实时完全相同的
//  Render 逐块推进 —— 只是把时钟从声卡回调换成 for 循环。这样“导出听到的”
//  与“现场听到的”在数学上必然一致（规格书 §7）。
// ==========================================================================
package main

import (
	"encoding/binary"
	"os"
	"sort"
)

// toPCM 把浮点样本夹到 [-1,1] 后转 int16（截断方式与 C 版一致）。
func toPCM(x float32) int16 {
	v := float64(x)
	if v < -1.0 {
		v = -1.0
	}
	if v > 1.0 {
		v = 1.0
	}
	return int16(v * 32767.0)
}

// totalSteps 计算一次导出要跑多少步。
//   song=true：按歌曲链累加（pattern 步数 × repeats）
//   song=false：只跑当前 pattern（一次）
func totalSteps(p *Project, song bool) int64 {
	var n int64
	if song && p.SongCount > 0 {
		for i := 0; i < int(p.SongCount) && i < 64; i++ {
			idx := p.Song[i].Pattern
			if idx < 0 || idx >= Patterns {
				idx = 0
			}
			rep := p.Song[i].Repeats
			if rep < 1 {
				rep = 1
			}
			steps := p.Patterns[idx].Steps
			if steps < 1 {
				steps = 1
			}
			n += int64(steps) * int64(rep)
		}
	} else {
		idx := p.Current
		if idx < 0 || idx >= Patterns {
			idx = 0
		}
		steps := p.Patterns[idx].Steps
		if steps < 1 {
			steps = 1
		}
		n = int64(steps)
	}
	if n < 1 {
		n = 16
	}
	return n
}

// ExportWAV 离线渲染到 16bit/48kHz/立体声 PCM。
//   song=true 走歌曲链；soloPad>=0 时只导出该轨（分轨用）。
func ExportWAV(p *Project, path string, song bool, soloPad int) bool {
	// 用工程副本渲染，避免离线导出把正在编辑的 current/songMode 改掉
	prj := *p
	if song {
		prj.Master.SongMode = 1
	} else {
		prj.Master.SongMode = 0
	}
	eng := NewEngine(&prj)
	eng.soloOverride = int32(soloPad)
	eng.startNow()

	steps := totalSteps(&prj, song)
	sl := int64(eng.StepLen())
	// 末尾多留 3 秒：给底鼓尾巴、开镲、拍手足够的自然衰减时间
	frames := steps*sl + 3*int64(XUSR)
	dataBytes := frames * 4

	f, err := os.Create(path)
	if err != nil {
		return false
	}
	defer f.Close()

	// ---- RIFF/WAVE 头（44 字节，标准 PCM） ----
	var hdr [44]byte
	copy(hdr[0:4], "RIFF")
	binary.LittleEndian.PutUint32(hdr[4:8], uint32(36+dataBytes))
	copy(hdr[8:16], "WAVEfmt ")
	binary.LittleEndian.PutUint32(hdr[16:20], 16)       // fmt 块长度
	binary.LittleEndian.PutUint16(hdr[20:22], 1)        // PCM
	binary.LittleEndian.PutUint16(hdr[22:24], 2)        // 双声道
	binary.LittleEndian.PutUint32(hdr[24:28], XUSR)     // 采样率
	binary.LittleEndian.PutUint32(hdr[28:32], XUSR*4)   // 字节率
	binary.LittleEndian.PutUint16(hdr[32:34], 4)        // 块对齐
	binary.LittleEndian.PutUint16(hdr[34:36], 16)       // 位深
	copy(hdr[36:40], "data")
	binary.LittleEndian.PutUint32(hdr[40:44], uint32(dataBytes))
	if _, err := f.Write(hdr[:]); err != nil {
		return false
	}

	// ---- 逐块渲染并落盘 ----
	L := make([]float32, BlockSize)
	R := make([]float32, BlockSize)
	buf := make([]byte, BlockSize*4)
	var done int64
	for done < frames {
		n := BlockSize
		if frames-done < int64(n) {
			n = int(frames - done)
		}
		eng.Render(n, L, R)
		for i := 0; i < n; i++ {
			l := toPCM(L[i])
			r := toPCM(R[i])
			buf[i*4+0] = byte(l)
			buf[i*4+1] = byte(uint16(l) >> 8)
			buf[i*4+2] = byte(r)
			buf[i*4+3] = byte(uint16(r) >> 8)
		}
		if _, err := f.Write(buf[:n*4]); err != nil {
			return false
		}
		done += int64(n)
	}
	return true
}

// --------------------------------------------------------------------------
//  MIDI 导出：SMF Format 1，PPQ=480，16 分音符 = 120 tick
// --------------------------------------------------------------------------
type midiNote struct {
	tick int
	note int
	vel  int
}

// vlq 写 MIDI 可变长度整数。
func vlq(out []byte, value int) int {
	if value < 0 {
		value = 0
	}
	var tmp [5]byte
	n := 0
	tmp[n] = byte(value & 0x7F)
	n++
	value >>= 7
	for value > 0 {
		tmp[n] = byte((value & 0x7F) | 0x80)
		n++
		value >>= 7
	}
	for i := 0; i < n; i++ {
		out[i] = tmp[n-1-i]
	}
	return n
}

// ExportMIDI 把音序展开成标准 MIDI 文件，可被任意 DAW 导入。
func ExportMIDI(p *Project, path string, song bool) bool {
	const ppq = 480
	bpm := clampF(p.Master.BPM, 40, 300)
	// 一步 = 1/16 音符 = 120 tick；micro 以样本为单位，需要按步长换算
	stepSamples := float64(XUSR) * 60.0 / bpm / 4.0

	// 歌曲序列（pattern 索引列表）
	var seq []int
	if song && p.SongCount > 0 {
		for i := 0; i < int(p.SongCount) && i < 64; i++ {
			idx := int(p.Song[i].Pattern)
			if idx < 0 || idx >= Patterns {
				idx = 0
			}
			rep := int(p.Song[i].Repeats)
			if rep < 1 {
				rep = 1
			}
			for k := 0; k < rep; k++ {
				seq = append(seq, idx)
			}
		}
	} else {
		idx := int(p.Current)
		if idx < 0 || idx >= Patterns {
			idx = 0
		}
		seq = append(seq, idx)
	}

	notes := make([]midiNote, 0, 16*64*8*4)
	var cursor int64 // 已走过的步数（跨 pattern 累加）
	for _, pi := range seq {
		pat := &p.Patterns[pi]
		steps := int(pat.Steps)
		if steps < 1 {
			steps = 1
		}
		for s := 0; s < steps; s++ {
			// swing 只影响奇数 16 分步，换算成 tick（120 * 0.5 = 60）
			swingTick := 0
			if s%2 == 1 {
				swingTick = int(p.Master.Swing * 60.0)
			}
			for pad := 0; pad < Pads; pad++ {
				tl := int(pat.TrackLen[pad])
				if tl < 1 {
					tl = 1
				}
				if tl > StepsMax {
					tl = StepsMax
				}
				idx := int((cursor + int64(s)) % int64(tl))
				st := &pat.Grid[pad][idx]
				if st.On == 0 || st.Prob == 0 {
					continue
				}
				vel := float64(st.Velocity)
				if st.Accent != 0 {
					vel *= 1.3
				}
				v := int(vel)
				v = clampI(v, 1, 127)
				baseTick := int((cursor + int64(s)) * 120)
				microTick := int(float64(st.Micro) / stepSamples * 120.0)
				r := int(st.Ratchet)
				r = clampI(r, 1, 8)
				for i := 0; i < r; i++ {
					tick := baseTick + swingTick + microTick + int(float64(i)*120.0/float64(r))
					if tick < 0 {
						tick = 0
					}
					notes = append(notes, midiNote{tick: tick, note: midiNotes[pad], vel: v})
				}
			}
		}
		cursor += int64(steps)
	}
	// 时间升序；同一 tick 按音高排，便于 DAW 显示
	sort.Slice(notes, func(i, j int) bool {
		if notes[i].tick != notes[j].tick {
			return notes[i].tick < notes[j].tick
		}
		return notes[i].note < notes[j].note
	})

	// ---- 构造 MTrk 事件流 ----
	track := make([]byte, 0, 1<<20)
	// 轨道名
	tn := 0
	var tmp [16]byte
	tn += vlq(tmp[:], 0)
	track = append(track, tmp[:tn]...)
	name := []byte("XU-808 by XuMou")
	track = append(track, 0xFF, 0x03)
	tn = vlq(tmp[:], len(name))
	track = append(track, tmp[:tn]...)
	track = append(track, name...)

	// 速度
	tempo := int(60000000.0 / bpm)
	track = appendTrackVlq(track, 0)
	track = append(track, 0xFF, 0x51, 0x03,
		byte(tempo>>16), byte(tempo>>8), byte(tempo))

	// 拍号
	track = appendTrackVlq(track, 0)
	track = append(track, 0xFF, 0x58, 0x04)
	bpb := clampI(int(p.Master.BeatsPerBar), 1, 16)
	track = append(track, byte(bpb))
	du := 0
	bu := int(p.Master.BeatUnit)
	for bu > 1 {
		bu >>= 1
		du++
	}
	track = append(track, byte(du), 24, 8)

	// 音符：每个音符 10 tick 长度（鼓机单击即可）
	last := 0
	for _, nt := range notes {
		track = appendTrackVlq(track, nt.tick-last)
		track = append(track, 0x99, byte(nt.note), byte(nt.vel))
		track = appendTrackVlq(track, 10)
		track = append(track, 0x89, byte(nt.note), 0x40)
		last = nt.tick + 10
	}
	// 轨道结束
	track = appendTrackVlq(track, 0)
	track = append(track, 0xFF, 0x2F, 0x00)

	// ---- 写文件 ----
	f, err := os.Create(path)
	if err != nil {
		return false
	}
	defer f.Close()
	head := []byte("MThd")
	head = binary.BigEndian.AppendUint32(head, 6)
	head = binary.BigEndian.AppendUint16(head, 1)    // Format 1
	head = binary.BigEndian.AppendUint16(head, 1)    // 1 条轨道
	head = binary.BigEndian.AppendUint16(head, ppq)  // PPQ 480
	if _, err := f.Write(head); err != nil {
		return false
	}
	tl := []byte("MTrk")
	tl = binary.BigEndian.AppendUint32(tl, uint32(len(track)))
	if _, err := f.Write(tl); err != nil {
		return false
	}
	_, err = f.Write(track)
	return err == nil
}

// appendTrackVlq 便捷函数：把可变长度整数追加到切片。
func appendTrackVlq(dst []byte, v int) []byte {
	var tmp [5]byte
	n := vlq(tmp[:], v)
	return append(dst, tmp[:n]...)
}

// ExportStems 导出 16 条分轨 WAV，文件名形如 "song_BD.wav"。
//  返回成功条数。
func ExportStems(p *Project, basePath string, song bool) int {
	ok := 0
	stem := basePath
	if len(stem) > 4 && (stem[len(stem)-4:] == ".wav" || stem[len(stem)-4:] == ".WAV") {
		stem = stem[:len(stem)-4]
	}
	for i := 0; i < Pads; i++ {
		name := stem + "_" + padCodes[i] + ".wav"
		if ExportWAV(p, name, song, i) {
			ok++
		}
	}
	return ok
}
