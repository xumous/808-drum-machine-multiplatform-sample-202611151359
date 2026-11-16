// ==========================================================================
//  main.go —— XU-808 Go 版入口
//
//  署名：徐某。
//  两种运行模式：
//    1. 正常启动：创建工程 + 引擎，交给 ui.go 的窗口消息循环；
//    2. --selftest：不开窗口、不出声，离线渲染 WAV/MIDI/JSON 并写结果标记，
//       供构建脚本在无人值守的情况下判断“DSP 和音序器真的算出了声音”。
// ==========================================================================
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strings"
)

func main() {
	if hasFlag("--selftest") {
		os.Exit(runSelfTest())
	}

	// 窗口过程必须在固定的 OS 线程上运行，否则消息泵与 GetMessage 会错线程
	runtime.LockOSThread()

	proj := NewProject()
	eng := NewEngine(proj)
	runUI(proj, eng)
}

// hasFlag 检查命令行是否带某个开关。
func hasFlag(flag string) bool {
	for _, s := range os.Args[1:] {
		if strings.EqualFold(s, flag) {
			return true
		}
	}
	return false
}

// runSelfTest 无界面自检：导出音频/MIDI/工程并写 SELFTEST 标记。
func runSelfTest() int {
	p := NewProject()

	okWav := ExportWAV(p, "selftest_go.wav", false, -1)
	okSong := ExportWAV(p, "selftest_go_song.wav", true, -1)
	okMidi := ExportMIDI(p, "selftest_go.mid", false)
	okSave := SaveProject(p, "selftest_go.json") == nil

	// 回读校验：确保“保存→加载”完整还原（规格书 §11.9）
	okLoad := false
	if okSave {
		var restored *Project = NewProject()
		if err := LoadProject(restored, "selftest_go.json"); err == nil {
			okLoad = restored.Patterns[0].Grid[0][0].On == 1 &&
				restored.Patterns[0].Steps == p.Patterns[0].Steps &&
				restored.Master.BPM == p.Master.BPM
		}
	}

	// 统计文件大小，顺便确认 WAV 真的非空
	wavSize := fileSize("selftest_go.wav")
	songSize := fileSize("selftest_go_song.wav")
	midiSize := fileSize("selftest_go.mid")
	jsonSize := fileSize("selftest_go.json")
	nonEmpty := wavSize > 44 && songSize > 44 && midiSize > 32 && jsonSize > 100

	// 16 声部逐一单轨渲染：任何一声部算出静音都算失败
	voiceOK := 0
	minPeak := 1.0
	for pad := 0; pad < Pads; pad++ {
		pk := padPeak(p, pad)
		if pk > 0.005 {
			voiceOK++
		}
		if pk < minPeak {
			minPeak = pk
		}
	}
	// 16 声部同时触发：软限幅必须保证峰值不超过 1（不削波）
	allPeak := allPadsPeak(p)

	all := okWav && okSong && okMidi && okSave && okLoad && nonEmpty &&
		voiceOK == Pads && allPeak > 0.2 && allPeak <= 1.0
	status := "SELFTEST FAIL"
	if all {
		status = "SELFTEST OK"
	}
	report := fmt.Sprintf(
		"%s\r\nwav=%d\r\nsong_wav=%d\r\nmidi=%d\r\njson=%d\r\nload=%v\r\nvoices=%d/%d\r\nmin_peak=%.4f\r\nall_peak=%.4f\r\n",
		status, wavSize, songSize, midiSize, jsonSize, okLoad, voiceOK, Pads, minPeak, allPeak)
	_ = os.WriteFile("selftest_go.txt", []byte(report), 0644)

	if all {
		return 0
	}
	return 1
}

// testPattern 构造一个只有指定声部在第 0 步触发的临时 pattern。
func testPattern(pad int, all bool) Pattern {
	pat := newPattern("TEST")
	for i := 0; i < Pads; i++ {
		if all || i == pad {
			pat.Grid[i][0].On = 1
			pat.Grid[i][0].Velocity = 120
		}
	}
	return pat
}

// padPeak 单独渲染某声部 2 秒，返回峰值（验证该声部真的会发声）。
func padPeak(p *Project, pad int) float64 {
	return renderPeak(p, testPattern(pad, false), pad)
}

// allPadsPeak 16 声部同时触发，返回峰值（验证软限幅不越界）。
func allPadsPeak(p *Project) float64 {
	return renderPeak(p, testPattern(0, true), -1)
}

// renderPeak 用离线引擎渲染 2 秒并取左声道峰值。
func renderPeak(p *Project, pat Pattern, solo int) float64 {
	prj := *p
	prj.Master.SongMode = 0
	prj.Current = 0
	prj.Patterns[0] = pat
	e := NewEngine(&prj)
	e.soloOverride = int32(solo)
	e.startNow()
	L := make([]float32, BlockSize)
	R := make([]float32, BlockSize)
	peak := 0.0
	for i := 0; i < (XUSR/BlockSize)*2; i++ {
		e.Render(BlockSize, L, R)
		for k := 0; k < BlockSize; k++ {
			if a := math.Abs(float64(L[k])); a > peak {
				peak = a
			}
		}
	}
	return peak
}

// fileSize 返回文件字节数；不存在返回 0。
func fileSize(path string) int64 {
	st, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return st.Size()
}
