// ==========================================================================
//  ui.go —— Win32 原生窗口 + 消息循环 + 输入（Go 版）
//
//  署名：徐某。
//  没有第三方 GUI 库：窗口类、双缓冲 GDI 绘制、鼠标键盘命中测试全部手写。
//  界面与音频彻底解耦 —— UI 卡顿只会让画面停顿，鼓点由声卡线程照常推进。
//
//  界面分区（1500×900 固定布局，与 Qt/C# 版视觉一致）：
//    · 顶部走带：LOGO / Play / Stop / BPM / Swing / Pattern / Steps / Song …
//    · 中部网格：16 声部 × N 步，左键开关、右键弹属性；
//    · 步进属性条：力度 / 重音 / 概率 / 微时 / 滚奏 / 条件；
//    · 声部参数：Tune/Decay/…/Level/Pan + M/S + 轨长；
//    · 主效果：高通 / 低通 / 共鸣 / Drive / 音量 / 电平表 / CPU。
// ==========================================================================
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// ---- 布局常量（改这里即可整体挪动分区） ----
const (
	uiW        = 1500
	uiH        = 900
	topH       = 96
	gridY      = 104
	gridHeadH  = 24
	cellH      = 22
	labelW     = 196
	stepEdY    = 490
	stepEdH    = 72
	paramY     = 570
	paramH     = 120
	masterY    = 698
	masterH    = 120
	footerY    = 826
	footerH    = 60
	gridRows   = Pads
)

// ---- 808 深色配色（规格书 §9） ----
var (
	cBg        = rgb(18, 18, 18)
	cPanel     = rgb(28, 28, 28)
	cPanel2    = rgb(35, 35, 35)
	cLine      = rgb(46, 46, 46)
	cOrange    = rgb(255, 122, 24)
	cOrangeD   = rgb(168, 74, 12)
	cYellow    = rgb(255, 209, 102)
	cCyan      = rgb(76, 201, 240)
	cText      = rgb(232, 232, 232)
	cDim       = rgb(138, 138, 138)
	cRed       = rgb(229, 72, 77)
	cGreen     = rgb(61, 220, 132)
	cBlack     = rgb(16, 16, 16)
	cCellOff   = rgb(16, 16, 16)
	cCellEdge  = rgb(39, 39, 39)
	cBarLine   = rgb(58, 58, 58)
	cRowEven   = rgb(22, 22, 22)
	cRowOdd    = rgb(26, 26, 26)
	cTrack     = rgb(12, 12, 12)
)

// overlay 类型
const (
	ovNone = 0
	ovStep = 1
	ovSong = 2
	ovSet  = 3
)

// 拖动类型
const (
	dragNone = 0
	dragPad  = 1
	dragMast = 2
	dragBPM  = 3
	dragSwing = 4
)

// r3 一组“减 / 值 / 加”三连按钮。
type r3 struct {
	minus, val, plus rect
}

// layout 所有可点击区域的矩形，paint 与输入共用同一份几何。
type layout struct {
	play, stop, tap                        rect
	bpmMinus, bpmVal, bpmPlus, bpmTrack    rect
	swMinus, swVal, swPlus, swTrack        rect
	patPrev, patVal, patNext               rect
	stepPrev, stepVal, stepNext            rect
	song, songEdit, settings, midiClk      rect
	save, load, expWav, expMidi, expStems  rect
	grid                                   rect
	stepEd                                 rect
	stepEdFields                           [7]r3
	params                                 rect
	master                                 rect
	footer                                 rect
	padMute, padSolo                       rect
	lenMinus, lenVal, lenPlus              rect
}

// uiState 整个 UI 的可变状态。
type uiState struct {
	hwnd uintptr
	inst uintptr
	proj *Project
	eng  *Engine

	audioOK bool
	midiOK  bool

	fonts struct {
		tiny, small, norm, bold, mono, logo, big uintptr
	}

	// 双缓冲 GDI 资源
	memDC  uintptr
	memBMP uintptr
	memW   int32
	memH   int32

	// 实际客户区尺寸（用于把设备坐标换算回 1500×900 设计坐标）
	clientW int32
	clientH int32

	selPad  int
	selStep int
	cellW   int32

	overlay      int
	popupX       int32 // 右键属性面板左上角
	popupY       int32
	songPage     int
	dragKind     int
	dragIdx      int
	status       string
	statusUntil  uint64
	lay          layout

	// Tap Tempo：最近 8 次点击间隔与上次点击时刻
	taps    []float64
	lastTap uint64
}

// app 全局单例：窗口过程是 C 回调，必须通过全局变量找到状态。
var app *uiState

// 窗口类名与标题必须是全局变量，保证指针在注册/创建期间不被回收。
var (
	windowClass = wstr("XU808_GO_WINDOW")
	windowTitle = wstr("XU-808 鼓机 - 徐某 (Go 版)")
)

// --------------------------------------------------------------------------
//  布局计算
// --------------------------------------------------------------------------
func buildLayout() layout {
	var L layout

	// 第一行：走带与节拍
	L.play = mkRect(210, 26, 78, 32)
	L.stop = mkRect(298, 26, 78, 32)
	L.tap = mkRect(386, 26, 52, 32)

	// BPM 组
	L.bpmMinus = mkRect(456, 26, 22, 32)
	L.bpmVal = mkRect(480, 26, 54, 32)
	L.bpmPlus = mkRect(536, 26, 22, 32)
	L.bpmTrack = mkRect(566, 32, 84, 20)

	// Swing 组
	L.swMinus = mkRect(660, 26, 22, 32)
	L.swVal = mkRect(684, 26, 54, 32)
	L.swPlus = mkRect(740, 26, 22, 32)
	L.swTrack = mkRect(770, 32, 84, 20)

	// Pattern 组
	L.patPrev = mkRect(864, 26, 26, 32)
	L.patVal = mkRect(894, 26, 132, 32)
	L.patNext = mkRect(1030, 26, 26, 32)

	// Steps 组
	L.stepPrev = mkRect(1064, 26, 26, 32)
	L.stepVal = mkRect(1094, 26, 54, 32)
	L.stepNext = mkRect(1152, 26, 26, 32)

	// 第二行：歌曲 / 文件 / 导出
	L.song = mkRect(14, 58, 80, 30)
	L.songEdit = mkRect(98, 58, 96, 30)
	L.settings = mkRect(198, 58, 70, 30)
	L.midiClk = mkRect(272, 58, 90, 30)
	L.save = mkRect(366, 58, 60, 30)
	L.load = mkRect(430, 58, 60, 30)
	L.expWav = mkRect(494, 58, 92, 30)
	L.expMidi = mkRect(590, 58, 98, 30)
	L.expStems = mkRect(692, 58, 92, 30)

	// 网格
	L.grid = mkRect(8, gridY, uiW-16, gridHeadH+gridRows*cellH)

	// 步进属性条
	L.stepEd = mkRect(8, stepEdY, uiW-16, stepEdH)
	for i := 0; i < 7; i++ {
		x := int32(16 + i*198)
		L.stepEdFields[i] = r3{
			minus: mkRect(x, 520, 22, 26),
			val:   mkRect(x+24, 520, 140, 26),
			plus:  mkRect(x+166, 520, 22, 26),
		}
	}

	// 声部参数
	L.params = mkRect(8, paramY, uiW-16, paramH)
	L.padMute = mkRect(1130, 574, 42, 20)
	L.padSolo = mkRect(1176, 574, 42, 20)
	L.lenMinus = mkRect(1276, 574, 22, 20)
	L.lenVal = mkRect(1300, 574, 34, 20)
	L.lenPlus = mkRect(1336, 574, 22, 20)

	// 主效果
	L.master = mkRect(8, masterY, uiW-16, masterH)
	L.footer = mkRect(8, footerY, uiW-16, footerH)
	return L
}

// 参数滑杆几何（声部面板）
func padSliderRect(i int) (label, track rect) {
	x := int32(16 + i*128)
	return mkRect(x, 596, 120, 14), mkRect(x, 616, 120, 8)
}

// 主效果滑杆几何
func masterSliderRect(i int) (label, track rect) {
	x := int32(16 + i*160)
	return mkRect(x, 726, 150, 14), mkRect(x, 744, 150, 8)
}

// 网格几何：格子宽度随步数自适应
func (a *uiState) layoutGrid() {
	pat := a.curPattern()
	steps := int(pat.Steps)
	if steps < 1 {
		steps = 1
	}
	avail := int32(uiW-8) - 8 - labelW
	cw := avail / int32(steps)
	if cw > 64 {
		cw = 64
	}
	if cw < 8 {
		cw = 8
	}
	a.cellW = cw
}

// updateClientSize 记录真实客户区大小；鼠标坐标要靠它换算回设计坐标。
func (a *uiState) updateClientSize() {
	var rc rect
	pGetClientRect.Call(a.hwnd, uintptr(unsafe.Pointer(&rc)))
	if rc.w() > 0 && rc.h() > 0 {
		a.clientW = rc.w()
		a.clientH = rc.h()
	}
}

// designX / designY：设备像素 → 1500×900 设计坐标。
func (a *uiState) designX(x int32) int32 {
	if a.clientW <= 0 {
		return x
	}
	return x * uiW / a.clientW
}

func (a *uiState) designY(y int32) int32 {
	if a.clientH <= 0 {
		return y
	}
	return y * uiH / a.clientH
}

// curPattern 当前 pattern（原子读，避免和音频线程的切歌动作撕裂）。
func (a *uiState) curPattern() *Pattern {
	idx := int(atomic.LoadInt32(&a.proj.Current))
	if idx < 0 || idx >= Patterns {
		idx = 0
	}
	return &a.proj.Patterns[idx]
}

// curSteps 当前 pattern 步数。
func (a *uiState) curSteps() int {
	s := int(atomic.LoadInt32(&a.curPattern().Steps))
	if s < 1 {
		s = 1
	}
	if s > StepsMax {
		s = StepsMax
	}
	return s
}

// selStepPtr 当前选中格子的指针。
func (a *uiState) selStepPtr() *Step {
	pat := a.curPattern()
	a.selPad = clampI(a.selPad, 0, Pads-1)
	a.selStep = clampI(a.selStep, 0, StepsMax-1)
	return &pat.Grid[a.selPad][a.selStep]
}

// setStatus 在底部显示一条临时消息。
func (a *uiState) setStatus(s string) {
	a.status = s
	now, _, _ := pGetTickCount64.Call()
	a.statusUntil = uint64(now) + 5000
}

// --------------------------------------------------------------------------
//  声部参数定义：不同声部暴露不同的旋钮（与 C 版一致）
// --------------------------------------------------------------------------
type padDef struct {
	label string
	get   func(*PadParams) *float64
	max   float64
}

func fTune(p *PadParams) *float64   { return &p.Tune }
func fDecay(p *PadParams) *float64  { return &p.Decay }
func fTone(p *PadParams) *float64   { return &p.Tone }
func fSnappy(p *PadParams) *float64 { return &p.Snappy }
func fTail(p *PadParams) *float64   { return &p.Tail }
func fLevel(p *PadParams) *float64  { return &p.Level }
func fPan(p *PadParams) *float64    { return &p.Pan }

var padDefsByPad [Pads][]padDef

// initPadDefs 按声部类别生成参数表；Level/Pan 每个声部都有。
func initPadDefs() {
	withLP := func(base ...padDef) []padDef {
		out := append([]padDef{}, base...)
		out = append(out, padDef{"LEVEL", fLevel, 1.25})
		out = append(out, padDef{"PAN", fPan, 1.0})
		return out
	}
	for i := 0; i < Pads; i++ {
		switch {
		case i == 0:
			padDefsByPad[i] = withLP(padDef{"TUNE", fTune, 1}, padDef{"DECAY", fDecay, 1}, padDef{"TONE", fTone, 1})
		case i == 1:
			padDefsByPad[i] = withLP(padDef{"TUNE", fTune, 1}, padDef{"SNAP", fSnappy, 1}, padDef{"DECAY", fDecay, 1})
		case i >= 2 && i <= 4:
			padDefsByPad[i] = withLP(padDef{"TUNE", fTune, 1}, padDef{"DECAY", fDecay, 1}, padDef{"COLOR", fTone, 1})
		case i == 10:
			padDefsByPad[i] = withLP(padDef{"DECAY", fDecay, 1}, padDef{"TONE", fTone, 1}, padDef{"TAIL", fTail, 1})
		case i == 11:
			padDefsByPad[i] = withLP(padDef{"DECAY", fDecay, 1}, padDef{"TONE", fTone, 1})
		case i == 12:
			padDefsByPad[i] = withLP(padDef{"TUNE", fTune, 1}, padDef{"DECAY", fDecay, 1})
		case i >= 13:
			padDefsByPad[i] = withLP(padDef{"DECAY", fDecay, 1}, padDef{"TONE", fTone, 1})
		default:
			padDefsByPad[i] = withLP(padDef{"TUNE", fTune, 1}, padDef{"DECAY", fDecay, 1})
		}
	}
}

// masterDef 主效果滑杆定义（线性映射到物理量）。
type masterDef struct {
	label    string
	get      func(*Master) *float64
	min, max float64
	unit     string
}

var masterDefs = []masterDef{
	{"LP 低通", func(m *Master) *float64 { return &m.LpCutoff }, 200, 20000, "Hz"},
	{"RES 共鸣", func(m *Master) *float64 { return &m.LpQ }, 0.5, 8, ""},
	{"HP 高通", func(m *Master) *float64 { return &m.HpCutoff }, 10, 800, "Hz"},
	{"DRIVE", func(m *Master) *float64 { return &m.Drive }, 1, 8, ""},
	{"VOL 音量", func(m *Master) *float64 { return &m.Volume }, 0, 1, ""},
}

// --------------------------------------------------------------------------
//  字体与窗口创建
// --------------------------------------------------------------------------
func makeFont(height int32, weight int, face string) uintptr {
	p := wstr(face)
	h, _, _ := pCreateFontW.Call(
		uintptr(height), 0, 0, 0, uintptr(weight),
		0, 0, 0, // 斜体/下划线/删除线
		1,       // DEFAULT_CHARSET
		0, 0, 5, // 输出精度/裁剪精度/CLEARTYPE_QUALITY
		0, uintptr(unsafe.Pointer(p)),
	)
	runtime.KeepAlive(p)
	return h
}

// runUI 创建窗口、启动音频/MIDI，然后进入消息循环。
func runUI(p *Project, e *Engine) {
	a := &uiState{proj: p, eng: e}
	app = a
	initPadDefs()
	a.lay = buildLayout()

	hInst, _, _ := pGetModuleHandleW.Call(0)
	a.inst = hInst

	// 高 DPI 下不做系统缩放，保持像素级布局一致
	pSetProcessDPIAware.Call()

	// 字体：全部用系统自带，绝不打包字体文件
	a.fonts.tiny = makeFont(-11, fwNormal, "Microsoft YaHei UI")
	a.fonts.small = makeFont(-12, fwNormal, "Microsoft YaHei UI")
	a.fonts.norm = makeFont(-13, fwNormal, "Microsoft YaHei UI")
	a.fonts.bold = makeFont(-13, fwBold, "Microsoft YaHei UI")
	a.fonts.mono = makeFont(-13, fwBold, "Consolas")
	a.fonts.logo = makeFont(-36, fwBold, "Consolas")
	a.fonts.big = makeFont(-18, fwBold, "Consolas")

	// 图标资源由 rsrc.syso 提供，组图标 ID = 1
	icon, _, _ := pLoadIconW.Call(hInst, 1)
	cursor, _, _ := pLoadCursorW.Call(0, idcArrow)

	wc := wndClassExW{
		cbSize:        uint32(unsafe.Sizeof(wndClassExW{})),
		style:         0x0020, // CS_HREDRAW | CS_VREDRAW
		lpfnWndProc:   syscall.NewCallback(wndProc),
		hInstance:     hInst,
		hIcon:         icon,
		hCursor:       cursor,
		hbrBackground: 0,
		lpszClassName: windowClass,
		hIconSm:       icon,
	}
	pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	// 固定尺寸窗口：布局按 1500×900 设计，再整体等比缩放到屏幕可用区域。
	// 这样 1366×768 的老笔记本和 4K 屏都能完整看到全部面板。
	style := uintptr(wsOverlappedWindow &^ wsThickFrame &^ wsMaximizeBox)
	sw, _, _ := pGetSystemMetrics.Call(0)
	sh, _, _ := pGetSystemMetrics.Call(1)
	scale := 1.0
	if s := float64(int32(sh)-72) / float64(uiH); s < scale {
		scale = s
	}
	if s := float64(int32(sw)-32) / float64(uiW); s < scale {
		scale = s
	}
	if scale < 0.6 {
		scale = 0.6
	}
	cw := int32(float64(uiW) * scale)
	ch := int32(float64(uiH) * scale)
	rc := rect{0, 0, cw, ch}
	pAdjustWindowRect.Call(uintptr(unsafe.Pointer(&rc)), style, 0)

	// 屏幕居中
	ww := rc.w()
	wh := rc.h()
	x := (int32(sw) - ww) / 2
	y := (int32(sh) - wh) / 2
	if x < 0 {
		x = 0
	}
	if y < 0 {
		y = 0
	}

	hwnd, _, _ := pCreateWindowExW.Call(
		0,
		uintptr(unsafe.Pointer(windowClass)),
		uintptr(unsafe.Pointer(windowTitle)),
		style,
		uintptr(x), uintptr(y), uintptr(ww), uintptr(wh),
		0, 0, hInst, 0,
	)
	if hwnd == 0 {
		return
	}
	a.hwnd = hwnd
	a.layoutGrid()
	a.updateClientSize()

	// 音频与 MIDI：失败也继续跑 UI，只是没声音/没输入
	a.audioOK = StartAudio(e)
	a.midiOK = StartMIDI()

	pShowWindow.Call(hwnd, swShow)
	pUpdateWindow.Call(hwnd)

	// 标准消息循环（GetMessage 0 = WM_QUIT）
	var msg msgT
	for {
		r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		pTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		pDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}

	// 退出后释放 GDI 资源，保证进程干净结束
	if a.memBMP != 0 {
		pDeleteObject.Call(a.memBMP)
		pDeleteDC.Call(a.memDC)
		a.memBMP, a.memDC = 0, 0
	}
}

// --------------------------------------------------------------------------
//  窗口过程
// --------------------------------------------------------------------------
func wndProc(hwnd, msg, wp, lp uintptr) uintptr {
	a := app
	if a == nil {
		r, _, _ := pDefWindowProcW.Call(hwnd, msg, wp, lp)
		return r
	}
	switch uint32(msg) {
	case wmCreate:
		a.hwnd = hwnd
		// 30fps 以上的 UI 刷新；只用于绘制与 MIDI 轮询，绝不当时钟
		pSetTimer.Call(hwnd, 1, 25, 0)
		return 0

	case wmSize:
		a.updateClientSize()
		a.layoutGrid()
		return 0

	case wmEraseBkgnd:
		return 1 // 自己绘制，不要系统擦背景，避免闪烁

	case wmTimer:
		a.pumpMIDI()
		if !a.audioOK {
			// 没有声卡时也要让播放/停止按钮有反馈
			a.eng.TickIdle()
		}
		pInvalidateRect.Call(hwnd, 0, 0)
		return 0

	case wmPaint:
		a.paint(hwnd)
		return 0

	case wmLButtonDown:
		a.onLDown(a.designX(getXLP(lp)), a.designY(getYLP(lp)))
		return 0

	case wmLButtonUp:
		if a.dragKind != dragNone {
			a.dragKind = dragNone
			a.dragIdx = 0
			pReleaseCapture.Call()
		}
		return 0

	case wmMouseMove:
		a.onMouseMove(a.designX(getXLP(lp)), a.designY(getYLP(lp)))
		return 0

	case wmRButtonDown:
		a.onRDown(a.designX(getXLP(lp)), a.designY(getYLP(lp)))
		return 0

	case wmKeyDown:
		a.onKey(int(wp))
		return 0

	case wmClose:
		pKillTimer.Call(hwnd, 1)
		a.eng.Stop()
		StopAudio()
		StopMIDI()
		pDestroyWindow.Call(hwnd)
		return 0

	case wmDestroy:
		pPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := pDefWindowProcW.Call(hwnd, msg, wp, lp)
	return r
}

// --------------------------------------------------------------------------
//  MIDI 事件分派（UI 线程，每 25ms 一次）
// --------------------------------------------------------------------------
func (a *uiState) pumpMIDI() {
	evs := TakeMIDIEvents()
	for _, ev := range evs {
		cmd := ev.st & 0xF0
		switch {
		case cmd == 0x90 && ev.d2 > 0: // Note On
			for pad := 0; pad < Pads; pad++ {
				if midiNotes[pad] == int(ev.d1) {
					a.eng.TriggerPad(pad, int(ev.d2), ev.d2 >= 120)
					break
				}
			}
		case cmd == 0xB0: // CC
			switch ev.d1 {
			case 7: // 主音量
				a.proj.Master.Volume = float64(ev.d2) / 127.0
			case 74: // 低通截止（对数映射 20Hz–20kHz）
				a.proj.Master.LpCutoff = 20.0 * math.Pow(1000.0, float64(ev.d2)/127.0)
			case 71: // 共鸣
				a.proj.Master.LpQ = 0.5 + float64(ev.d2)/127.0*7.5
			}
		case cmd == 0xC0: // Program Change → 切 Pattern
			a.eng.RequestPattern(int(ev.d2) % Patterns)
			a.layoutGrid()
		case ev.st == 0xFA: // Start
			a.eng.Play()
		case ev.st == 0xFC: // Stop
			a.eng.Stop()
		case ev.st == 0xF8: // 时钟
			a.eng.ExternalPulse()
		}
	}
}

// --------------------------------------------------------------------------
//  鼠标左键
// --------------------------------------------------------------------------
func (a *uiState) onLDown(x, y int32) {
	// 弹层优先：点弹层内部处理，点外部则关闭
	if a.overlay != ovNone {
		a.overlayClick(x, y)
		return
	}

	pat := a.curPattern()
	L := a.lay

	// ---- 第一行 ----
	if L.play.contains(x, y) {
		a.eng.Play()
		return
	}
	if L.stop.contains(x, y) {
		a.eng.Stop()
		return
	}
	if L.tap.contains(x, y) {
		a.tapTempo()
		return
	}
	if L.bpmMinus.contains(x, y) {
		a.proj.Master.BPM = clampF(a.proj.Master.BPM-1, 40, 300)
		return
	}
	if L.bpmPlus.contains(x, y) {
		a.proj.Master.BPM = clampF(a.proj.Master.BPM+1, 40, 300)
		return
	}
	if hitTrack(L.bpmTrack, x, y) {
		a.dragKind = dragBPM
		pSetCapture.Call(a.hwnd)
		a.setBPMFromX(x)
		return
	}
	if L.swMinus.contains(x, y) {
		a.proj.Master.Swing = clampF(a.proj.Master.Swing-0.01, 0, 0.75)
		return
	}
	if L.swPlus.contains(x, y) {
		a.proj.Master.Swing = clampF(a.proj.Master.Swing+0.01, 0, 0.75)
		return
	}
	if hitTrack(L.swTrack, x, y) {
		a.dragKind = dragSwing
		pSetCapture.Call(a.hwnd)
		a.setSwingFromX(x)
		return
	}
	if L.patPrev.contains(x, y) {
		a.eng.RequestPattern((int(atomic.LoadInt32(&a.proj.Current)) - 1 + Patterns) % Patterns)
		a.layoutGrid()
		return
	}
	if L.patNext.contains(x, y) {
		a.eng.RequestPattern((int(atomic.LoadInt32(&a.proj.Current)) + 1) % Patterns)
		a.layoutGrid()
		return
	}
	if L.stepPrev.contains(x, y) {
		if pat.Steps > 1 {
			atomic.StoreInt32(&pat.Steps, pat.Steps-1)
			a.selStep = clampI(a.selStep, 0, int(pat.Steps)-1)
			a.layoutGrid()
		}
		return
	}
	if L.stepNext.contains(x, y) {
		if pat.Steps < StepsMax {
			atomic.StoreInt32(&pat.Steps, pat.Steps+1)
			a.layoutGrid()
		}
		return
	}

	// ---- 第二行 ----
	if L.song.contains(x, y) {
		a.proj.Master.SongMode = 1 - a.proj.Master.SongMode
		return
	}
	if L.songEdit.contains(x, y) {
		a.overlay = ovSong
		return
	}
	if L.settings.contains(x, y) {
		a.overlay = ovSet
		return
	}
	if L.midiClk.contains(x, y) {
		a.eng.clockExternal.Store(1 - a.eng.clockExternal.Load())
		return
	}
	if L.save.contains(x, y) {
		a.actionSave()
		return
	}
	if L.load.contains(x, y) {
		a.actionLoad()
		return
	}
	if L.expWav.contains(x, y) {
		a.actionExportWAV()
		return
	}
	if L.expMidi.contains(x, y) {
		a.actionExportMIDI()
		return
	}
	if L.expStems.contains(x, y) {
		a.actionExportStems()
		return
	}

	// ---- 步进属性条 ----
	for i := 0; i < 7; i++ {
		f := L.stepEdFields[i]
		if f.minus.contains(x, y) {
			a.stepAdjust(i, -1)
			return
		}
		if f.plus.contains(x, y) {
			a.stepAdjust(i, +1)
			return
		}
		if f.val.contains(x, y) && (i == 0 || i == 1) {
			a.stepAdjust(i, +1)
			return
		}
	}

	// ---- 网格行标签：M / S / 选中 ----
	gx := L.grid.left
	gy := L.grid.top + gridHeadH
	if x >= gx && x < gx+labelW && y >= gy && y < gy+gridRows*cellH {
		r := int((y - gy) / cellH)
		mx := gx + 110
		muteR := mkRect(mx, gy+int32(r)*cellH+3, 22, cellH-6)
		soloR := mkRect(mx+26, gy+int32(r)*cellH+3, 22, cellH-6)
		if muteR.contains(x, y) {
			a.proj.Pads[r].Mute = 1 - a.proj.Pads[r].Mute
			return
		}
		if soloR.contains(x, y) {
			a.proj.Pads[r].Solo = 1 - a.proj.Pads[r].Solo
			return
		}
		a.selPad = r
		a.eng.TriggerPad(r, 112, false) // 点标签试听
		return
	}

	// ---- 网格格子：左键开关 ----
	gcx := gx + labelW
	if x >= gcx && x < gcx+int32(a.curSteps())*a.cellW && y >= gy && y < gy+gridRows*cellH {
		c := int((x - gcx) / a.cellW)
		r := int((y - gy) / cellH)
		if c >= 0 && c < a.curSteps() && r >= 0 && r < Pads {
			st := &pat.Grid[r][c]
			if st.On != 0 {
				st.On = 0
			} else {
				st.On = 1
			}
			a.selPad = r
			a.selStep = c
			if st.On != 0 {
				// 打开即试听一次，方便调音色
				a.eng.TriggerPad(r, int(st.Velocity), st.Accent != 0)
			}
		}
		return
	}

	// ---- 声部参数区 ----
	if L.padMute.contains(x, y) {
		a.proj.Pads[a.selPad].Mute = 1 - a.proj.Pads[a.selPad].Mute
		return
	}
	if L.padSolo.contains(x, y) {
		a.proj.Pads[a.selPad].Solo = 1 - a.proj.Pads[a.selPad].Solo
		return
	}
	if L.lenMinus.contains(x, y) || L.lenPlus.contains(x, y) {
		d := int32(-1)
		if L.lenPlus.contains(x, y) {
			d = 1
		}
		v := int32(pat.TrackLen[a.selPad]) + d
		if v < 1 {
			v = 1
		}
		if v > StepsMax {
			v = StepsMax
		}
		pat.TrackLen[a.selPad] = uint8(v)
		return
	}

	defs := padDefsByPad[a.selPad]
	for i := range defs {
		_, track := padSliderRect(i)
		if hitTrack(track, x, y) {
			a.dragKind = dragPad
			a.dragIdx = i
			pSetCapture.Call(a.hwnd)
			a.setPadFromX(i, x)
			return
		}
	}

	// ---- 主效果区 ----
	for i := range masterDefs {
		_, track := masterSliderRect(i)
		if hitTrack(track, x, y) {
			a.dragKind = dragMast
			a.dragIdx = i
			pSetCapture.Call(a.hwnd)
			a.setMasterFromX(i, x)
			return
		}
	}
}

// hitTrack 滑杆命中区（纵向放宽 10px，方便点击）。
func hitTrack(track rect, x, y int32) bool {
	r := rect{track.left, track.top - 8, track.right, track.bottom + 8}
	return r.contains(x, y)
}

// sliderValue 把鼠标 x 换算成 0..1。
func sliderValue(track rect, x int32) float64 {
	w := track.w()
	if w <= 0 {
		return 0
	}
	v := float64(x-track.left) / float64(w)
	return clampF(v, 0, 1)
}

func (a *uiState) setBPMFromX(x int32) {
	v := sliderValue(a.lay.bpmTrack, x)
	a.proj.Master.BPM = 40 + v*260
}

func (a *uiState) setSwingFromX(x int32) {
	v := sliderValue(a.lay.swTrack, x)
	a.proj.Master.Swing = v * 0.75
}

func (a *uiState) setPadFromX(i int, x int32) {
	defs := padDefsByPad[a.selPad]
	if i < 0 || i >= len(defs) {
		return
	}
	_, track := padSliderRect(i)
	v := sliderValue(track, x)
	*defs[i].get(&a.proj.Pads[a.selPad]) = v * defs[i].max
}

func (a *uiState) setMasterFromX(i int, x int32) {
	if i < 0 || i >= len(masterDefs) {
		return
	}
	_, track := masterSliderRect(i)
	v := sliderValue(track, x)
	d := masterDefs[i]
	*d.get(&a.proj.Master) = d.min + v*(d.max-d.min)
}

// onMouseMove 处理滑杆拖动。
func (a *uiState) onMouseMove(x, y int32) {
	switch a.dragKind {
	case dragBPM:
		a.setBPMFromX(x)
	case dragSwing:
		a.setSwingFromX(x)
	case dragPad:
		a.setPadFromX(a.dragIdx, x)
	case dragMast:
		a.setMasterFromX(a.dragIdx, x)
	}
}

// onRDown 右键：格子上弹出“步进属性”。
func (a *uiState) onRDown(x, y int32) {
	L := a.lay
	gy := L.grid.top + gridHeadH
	gcx := L.grid.left + labelW
	if x >= gcx && x < gcx+int32(a.curSteps())*a.cellW && y >= gy && y < gy+gridRows*cellH {
		c := int((x - gcx) / a.cellW)
		r := int((y - gy) / cellH)
		if c >= 0 && c < a.curSteps() && r >= 0 && r < Pads {
			a.selPad = r
			a.selStep = c
			a.popupX = x + 8
			a.popupY = y + 8
			a.overlay = ovStep
		}
	}
}

// onKey 键盘：空格走带、1-8 切 Pattern、ZXCV… 打鼓、ESC 关弹层。
func (a *uiState) onKey(vk int) {
	if vk == vkEscape {
		a.overlay = ovNone
		return
	}
	if vk == vkSpace {
		if a.eng.playing.Load() != 0 {
			a.eng.Stop()
		} else {
			a.eng.Play()
		}
		return
	}
	if vk >= '1' && vk <= '8' {
		a.eng.RequestPattern(vk - '1')
		a.layoutGrid()
		return
	}
	// 16 键鼓垫：Z X C V B N M A S D F G H J K L
	keys := "ZXCVBNMASDFGHJKL"
	for i := 0; i < len(keys); i++ {
		if int(keys[i]) == vk {
			a.eng.TriggerPad(i, 110, false)
			return
		}
	}
}

// tapTempo 打拍定速：取最近 8 次间隔，去掉最大最小后平均。
func (a *uiState) tapTempo() {
	now, _, _ := pGetTickCount64.Call()
	now64 := uint64(now)
	if a.lastTap != 0 && now64-a.lastTap < 2000 {
		iv := float64(now64-a.lastTap) / 1000.0
		if len(a.taps) < 8 {
			a.taps = append(a.taps, iv)
		} else {
			a.taps = append(a.taps[1:], iv)
		}
		if len(a.taps) >= 2 {
			// 去掉最大和最小，抗一次手抖
			mn, mx := a.taps[0], a.taps[0]
			sum := 0.0
			for _, v := range a.taps {
				sum += v
				if v < mn {
					mn = v
				}
				if v > mx {
					mx = v
				}
			}
			avg := (sum - mn - mx) / float64(len(a.taps)-2)
			if avg > 0 {
				bpm := 60.0 / avg
				if bpm >= 30 && bpm <= 400 {
					a.proj.Master.BPM = clampF(bpm, 40, 300)
				}
			}
		}
	} else {
		a.taps = a.taps[:0]
	}
	a.lastTap = now64
}

// 打拍状态直接放在 uiState 里（taps / lastTap）。

// --------------------------------------------------------------------------
//  步进属性条的自增自减
// --------------------------------------------------------------------------
func (a *uiState) stepAdjust(idx, delta int) {
	st := a.selStepPtr()
	switch idx {
	case 0:
		st.On = 1 - st.On
	case 1:
		st.Accent = 1 - st.Accent
	case 2: // 力度
		v := int(st.Velocity) + delta
		st.Velocity = uint8(clampI(v, 1, 127))
	case 3: // 概率
		v := int(st.Prob) + delta
		st.Prob = uint8(clampI(v, 0, 100))
	case 4: // 微时（每格 10 样本）
		v := int(st.Micro) + delta*10
		st.Micro = int16(clampI(v, -2400, 2400))
	case 5: // 滚奏
		v := int(st.Ratchet) + delta
		st.Ratchet = uint8(clampI(v, 1, 8))
	case 6: // 条件
		v := (int(st.Cond) + delta + 5) % 5
		st.Cond = uint8(v)
	}
}

// --------------------------------------------------------------------------
//  文件操作
// --------------------------------------------------------------------------
func (a *uiState) actionSave() {
	filter := utf16Filter("XU-808 工程", "*.json;*.xu808", "所有文件", "*.*")
	path, ok := saveFileDialog(a.hwnd, "xu808_go.json", filter, "json")
	if !ok {
		return
	}
	if err := SaveProject(a.proj, path); err != nil {
		a.setStatus("保存失败: " + err.Error())
	} else {
		a.setStatus("已保存: " + path)
	}
}

func (a *uiState) actionLoad() {
	filter := utf16Filter("XU-808 工程", "*.json;*.xu808", "所有文件", "*.*")
	path, ok := openFileDialog(a.hwnd, "xu808_go.json", filter, "json")
	if !ok {
		return
	}
	// 必须先停音频：回调线程正在读工程，直接覆盖会读到半截数据
	StopAudio()
	a.eng.Stop()
	if err := LoadProject(a.proj, path); err != nil {
		a.setStatus("打开失败: " + err.Error())
		StartAudio(a.eng)
		a.audioOK = audio.hwo != 0
		return
	}
	// 重建引擎：噪声池/平滑状态/事件队列全部归零，避免旧状态残留
	a.eng = NewEngine(a.proj)
	a.audioOK = StartAudio(a.eng)
	a.layoutGrid()
	a.selPad, a.selStep = 0, 0
	a.setStatus("已打开: " + path)
}

func (a *uiState) actionExportWAV() {
	filter := utf16Filter("WAV 音频", "*.wav")
	path, ok := saveFileDialog(a.hwnd, "xu808_go.wav", filter, "wav")
	if !ok {
		return
	}
	song := a.proj.Master.SongMode != 0
	if ExportWAV(a.proj, path, song, -1) {
		a.setStatus("已导出 WAV: " + path)
	} else {
		a.setStatus("导出 WAV 失败: " + path)
	}
}

func (a *uiState) actionExportMIDI() {
	filter := utf16Filter("MIDI 文件", "*.mid")
	path, ok := saveFileDialog(a.hwnd, "xu808_go.mid", filter, "mid")
	if !ok {
		return
	}
	song := a.proj.Master.SongMode != 0
	if ExportMIDI(a.proj, path, song) {
		a.setStatus("已导出 MIDI: " + path)
	} else {
		a.setStatus("导出 MIDI 失败: " + path)
	}
}

func (a *uiState) actionExportStems() {
	filter := utf16Filter("WAV 分轨（基名）", "*.wav")
	path, ok := saveFileDialog(a.hwnd, "xu808_stems.wav", filter, "wav")
	if !ok {
		return
	}
	song := a.proj.Master.SongMode != 0
	n := ExportStems(a.proj, path, song)
	if n > 0 {
		a.setStatus(fmt.Sprintf("已导出 %d 条分轨（%s_BD.wav …）", n, trimExt(path)))
	} else {
		a.setStatus("分轨导出失败")
	}
}

// trimExt 去掉扩展名，用于拼分轨文件名提示。
func trimExt(p string) string {
	if len(p) > 4 {
		ext := p[len(p)-4:]
		if ext == ".wav" || ext == ".WAV" {
			return p[:len(p)-4]
		}
	}
	return p
}

// --------------------------------------------------------------------------
//  其它小工具
// --------------------------------------------------------------------------
// condName 条件触发的中文名。
func condName(c uint8) string {
	switch c {
	case CondAlways:
		return "总是"
	case CondFill:
		return "过门"
	case CondNotFill:
		return "非过门"
	case CondEvery2:
		return "每2"
	case CondEvery4:
		return "每4"
	}
	return "总是"
}

// fileExists 判断文件是否存在（用于首次启动提示）。
func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
