// ==========================================================================
//  ui_paint.go —— 全部界面绘制（GDI 手绘，双缓冲）
//
//  署名：徐某。
//  绘制顺序：背景 → 顶部走带 → 网格 → 步进属性 → 声部参数 → 主效果
//  → 底部状态 → 弹层。所有颜色、坐标都集中在这里，方便与其它语言版对齐。
// ==========================================================================
package main

import (
	"fmt"
	"sync/atomic"
	"unsafe"
)

// paint 双缓冲绘制：先画到固定 1500×900 的内存位图，再 StretchBlt 到窗口。
// 这样无论屏幕是 1080p 还是 768p，界面比例都完全一致。
func (a *uiState) paint(hwnd uintptr) {
	var ps paintStruct
	hdc, _, _ := pBeginPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
	var rc rect
	pGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&rc)))
	w, h := rc.w(), rc.h()

	// 内存 DC 固定为设计分辨率，只在第一次创建
	if a.memDC == 0 {
		a.memDC, _, _ = pCreateCompatibleDC.Call(hdc)
		a.memBMP, _, _ = pCreateCompatibleBitmap.Call(hdc, uiW, uiH)
		a.memW, a.memH = uiW, uiH
	}
	old, _, _ := pSelectObject.Call(a.memDC, a.memBMP)

	a.paintAll(a.memDC, uiW, uiH)

	// COLORONCOLOR(3) 缩放在速度与清晰度之间最平衡
	pSetStretchBltMode.Call(hdc, 3)
	pStretchBlt.Call(
		hdc, 0, 0, uintptr(w), uintptr(h),
		a.memDC, 0, 0, uintptr(uiW), uintptr(uiH),
		srcCopy,
	)
	pSelectObject.Call(a.memDC, old)
	pEndPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
}

// paintAll 绘制所有分区。
func (a *uiState) paintAll(hdc uintptr, w, h int32) {
	fillRect(hdc, rect{0, 0, w, h}, cBg)
	a.paintTop(hdc)
	a.paintGrid(hdc)
	a.paintStepEditor(hdc)
	a.paintParams(hdc)
	a.paintMaster(hdc)
	a.paintFooter(hdc)
	// 弹层最后画，保证盖在最上面
	switch a.overlay {
	case ovStep:
		a.paintStepPopup(hdc)
	case ovSong:
		a.paintSongEditor(hdc)
	case ovSet:
		a.paintSettings(hdc)
	}
}

// --------------------------------------------------------------------------
//  通用控件
// --------------------------------------------------------------------------

// button 画一个按钮。
func (a *uiState) button(hdc uintptr, r rect, label string, active, enabled bool) {
	bg := cPanel2
	bd := cLine
	if active {
		bg = cOrangeD
		bd = cOrange
	}
	if !enabled {
		bg = cPanel
	}
	fillRect(hdc, r, bg)
	frameRect(hdc, r, bd, 1)
	tc := cText
	if !enabled {
		tc = cDim
	}
	drawText(hdc, r, label, tc, a.fonts.bold, dtCenter)
}

// tinyToggle 画网格行内的 M / S 小按钮。
func (a *uiState) tinyToggle(hdc uintptr, r rect, label string, on bool, onColor uintptr) {
	bg := uintptr(rgb(42, 42, 42))
	tc := cDim
	if on {
		bg = onColor
		tc = cBg
	}
	fillRect(hdc, r, bg)
	if !on {
		frameRect(hdc, r, cLine, 1)
	}
	drawText(hdc, r, label, tc, a.fonts.tiny, dtCenter)
}

// sliderTrack 画滑杆轨道、已填充部分与手柄。
func sliderTrack(hdc uintptr, track rect, v01 float64) {
	fillRect(hdc, track, cTrack)
	fillRect(hdc, rect{track.left + 1, track.top + 1, track.right - 1, track.bottom - 1}, rgb(24, 24, 24))
	v01 = clampF(v01, 0, 1)
	w := track.w()
	hx := track.left + int32(v01*float64(w-10))
	fillRect(hdc, rect{track.left, track.top, hx, track.bottom}, cOrangeD)
	handle := rect{hx, track.top - 5, hx + 10, track.bottom + 5}
	fillRect(hdc, handle, cOrange)
	frameRect(hdc, handle, rgb(20, 20, 20), 1)
}

// stepValueBox 画“减 / 值 / 加”里的值框。
func (a *uiState) stepValueBox(hdc uintptr, r rect, text string) {
	fillRect(hdc, r, cBlack)
	frameRect(hdc, r, cLine, 1)
	drawText(hdc, r, text, cText, a.fonts.bold, dtCenter)
}

// blendColor 把基色按亮度 a（0..255）与暗色混合，用于力度可视化。
func blendColor(base uintptr, a int) uintptr {
	if a < 0 {
		a = 0
	}
	if a > 255 {
		a = 255
	}
	r := int(base & 0xFF)
	g := int((base >> 8) & 0xFF)
	b := int((base >> 16) & 0xFF)
	rr := (r*a + 18*(255-a)) / 255
	gg := (g*a + 18*(255-a)) / 255
	bb := (b*a + 18*(255-a)) / 255
	return rgb(uint32(rr), uint32(gg), uint32(bb))
}

// --------------------------------------------------------------------------
//  顶部走带
// --------------------------------------------------------------------------
func (a *uiState) paintTop(hdc uintptr) {
	fillRect(hdc, mkRect(0, 0, uiW, topH), cPanel)
	line(hdc, 0, topH-1, uiW, topH-1, cLine, 1)

	// LOGO 与署名
	drawText(hdc, mkRect(14, 2, 210, 44), "XU-808", cOrange, a.fonts.logo, dtLeft)
	drawText(hdc, mkRect(16, 46, 320, 14), "RHYTHM COMPOSER · 徐某 · 16 VOICE",
		cDim, a.fonts.tiny, dtLeft)

	L := a.lay
	m := &a.proj.Master
	pat := a.curPattern()
	playing := a.eng.playing.Load() != 0

	// 第一行按钮
	a.button(hdc, L.play, "▶ PLAY", playing, true)
	a.button(hdc, L.stop, "■ STOP", false, true)
	a.button(hdc, L.tap, "TAP", false, true)

	// BPM
	drawText(hdc, mkRect(456, 8, 120, 14), "BPM", cDim, a.fonts.tiny, dtLeft)
	a.button(hdc, L.bpmMinus, "-", false, true)
	a.button(hdc, L.bpmPlus, "+", false, true)
	a.stepValueBox(hdc, L.bpmVal, fmt.Sprintf("%.1f", m.BPM))
	sliderTrack(hdc, L.bpmTrack, (m.BPM-40)/260.0)
	drawText(hdc, mkRect(L.bpmTrack.left, L.bpmTrack.bottom+2, 84, 12),
		"40-300", cDim, a.fonts.tiny, dtLeft)

	// SWING
	drawText(hdc, mkRect(660, 8, 120, 14), "SWING 摇摆", cDim, a.fonts.tiny, dtLeft)
	a.button(hdc, L.swMinus, "-", false, true)
	a.button(hdc, L.swPlus, "+", false, true)
	a.stepValueBox(hdc, L.swVal, fmt.Sprintf("%d%%", int(m.Swing*100)))
	sliderTrack(hdc, L.swTrack, m.Swing/0.75)

	// PATTERN
	drawText(hdc, mkRect(864, 8, 160, 14), "PATTERN", cDim, a.fonts.tiny, dtLeft)
	a.button(hdc, L.patPrev, "<", false, true)
	a.button(hdc, L.patNext, ">", false, true)
	pv := mkRect(L.patVal.left, L.patVal.top, L.patVal.w(), L.patVal.h())
	fillRect(hdc, pv, cBlack)
	frameRect(hdc, pv, cLine, 1)
	drawText(hdc, pv, fmt.Sprintf("%02d  %s", atomic.LoadInt32(&a.proj.Current)+1, pat.Name),
		cText, a.fonts.norm, dtCenter)

	// STEPS
	drawText(hdc, mkRect(1064, 8, 100, 14), "STEPS", cDim, a.fonts.tiny, dtLeft)
	a.button(hdc, L.stepPrev, "-", false, true)
	a.button(hdc, L.stepNext, "+", false, true)
	a.stepValueBox(hdc, L.stepVal, fmt.Sprintf("%d", atomic.LoadInt32(&pat.Steps)))

	// 拍号 / 采样率
	drawText(hdc, mkRect(1200, 10, 290, 16),
		fmt.Sprintf("拍号 %d/%d   ·   48000 Hz", m.BeatsPerBar, m.BeatUnit),
		cDim, a.fonts.tiny, dtRight)

	// 第二行
	a.button(hdc, L.song, "SONG", m.SongMode != 0, true)
	a.button(hdc, L.songEdit, "歌曲链", false, true)
	a.button(hdc, L.settings, "设置", false, true)
	a.button(hdc, L.midiClk, "MIDI CLK", a.eng.clockExternal.Load() != 0, true)
	a.button(hdc, L.save, "保存", false, true)
	a.button(hdc, L.load, "打开", false, true)
	a.button(hdc, L.expWav, "导出WAV", false, true)
	a.button(hdc, L.expMidi, "导出MIDI", false, true)
	a.button(hdc, L.expStems, "导出分轨", false, true)

	// 硬件状态
	audioTxt := "无音频"
	if a.audioOK {
		audioTxt = "48kHz 立体声"
	}
	midiTxt := "MIDI -"
	if a.midiOK {
		midiTxt = "MIDI ON"
	}
	drawText(hdc, mkRect(880, 58, 604, 30),
		fmt.Sprintf("%s   ·   %s   ·   块 %d", midiTxt, audioTxt, BlockSize),
		cDim, a.fonts.tiny, dtRight)
}

// --------------------------------------------------------------------------
//  16×N 步进网格
// --------------------------------------------------------------------------
func (a *uiState) paintGrid(hdc uintptr) {
	steps := a.curSteps()
	pat := a.curPattern()
	gx := a.lay.grid.left
	gy := a.lay.grid.top
	cw := a.cellW
	headH := int32(gridHeadH)
	rowH := int32(cellH)
	gridRight := gx + labelW + int32(steps)*cw
	gridBottom := gy + headH + gridRows*rowH

	// 表头
	fillRect(hdc, rect{gx, gy, gridRight, gy + headH}, cPanel)
	drawText(hdc, mkRect(gx+8, gy, labelW-16, headH), "声部 / STEP", cDim, a.fonts.tiny, dtLeft)

	// 步号：格子太窄时每 4 步标一次，避免字叠在一起
	if cw >= 18 {
		for c := 0; c < steps; c++ {
			x := gx + labelW + int32(c)*cw
			col := cDim
			if c%4 == 0 {
				col = cText
			}
			drawText(hdc, rect{x, gy, x + cw, gy + headH}, fmt.Sprintf("%d", c+1),
				col, a.fonts.tiny, dtCenter)
		}
	} else {
		for c := 0; c < steps; c += 4 {
			x := gx + labelW + int32(c)*cw
			drawText(hdc, rect{x, gy, x + cw*4, gy + headH}, fmt.Sprintf("%d", c+1),
				cDim, a.fonts.tiny, dtCenter)
		}
	}

	// 行标签列背景
	fillRect(hdc, rect{gx, gy + headH, gx + labelW, gridBottom}, cPanel2)

	for r := 0; r < gridRows; r++ {
		y := gy + headH + int32(r)*rowH
		bg := cRowEven
		if r%2 == 1 {
			bg = cRowOdd
		}
		fillRect(hdc, rect{gx + labelW, y, gridRight, y + rowH}, bg)

		// 选中行左侧橙条
		if r == a.selPad {
			fillRect(hdc, rect{gx, y, gx + 4, y + rowH}, cOrange)
		}

		// 代号 / 中文名
		drawText(hdc, mkRect(gx+8, y, 36, rowH), padCodes[r], cOrange, a.fonts.mono, dtLeft)
		drawText(hdc, mkRect(gx+48, y, 62, rowH), padNamesCN[r], cText, a.fonts.tiny, dtLeft)

		// M / S
		muteR := mkRect(gx+110, y+3, 22, rowH-6)
		soloR := mkRect(gx+136, y+3, 22, rowH-6)
		a.tinyToggle(hdc, muteR, "M", atomic.LoadInt32(&a.proj.Pads[r].Mute) != 0, cRed)
		a.tinyToggle(hdc, soloR, "S", atomic.LoadInt32(&a.proj.Pads[r].Solo) != 0, cYellow)

		// 轨长（polymeter）
		drawText(hdc, mkRect(gx+160, y, 34, rowH),
			fmt.Sprintf("%d", pat.TrackLen[r]), cDim, a.fonts.tiny, dtRight)

		// 格子
		for c := 0; c < steps; c++ {
			cx := gx + labelW + int32(c)*cw
			cell := rect{cx + 2, y + 2, cx + cw - 2, y + rowH - 2}
			st := &pat.Grid[r][c]
			if st.On != 0 {
				amp := 110 + int(145*float64(st.Velocity)/127.0)
				base := cOrange
				if st.Accent != 0 {
					base = cYellow
				}
				fillRect(hdc, cell, blendColor(base, amp))
				if st.Prob < 100 || st.Ratchet > 1 {
					mark := "%"
					if st.Ratchet > 1 {
						mark = "R"
					}
					drawText(hdc, cell, mark, rgb(26, 16, 8), a.fonts.tiny, dtCenter)
				}
			} else {
				fillRect(hdc, cell, cCellOff)
				frameRect(hdc, cell, cCellEdge, 1)
			}
			// 当前选中格子
			if c == a.selStep && r == a.selPad {
				frameRect(hdc, cell, cCyan, 1)
			}
		}
	}

	// 每 4 步一条分隔线
	for c := 0; c <= steps; c += 4 {
		x := gx + labelW + int32(c)*cw
		if x <= gridRight {
			line(hdc, x, gy+headH, x, gridBottom, cBarLine, 1)
		}
	}

	// 播放头：两条青色竖线标出当前列
	if a.eng.playing.Load() != 0 {
		idx := int(a.eng.stepIndex.Load())
		if idx >= 0 && idx < steps {
			x := gx + labelW + int32(idx)*cw
			line(hdc, x, gy+headH, x, gridBottom, cCyan, 2)
			line(hdc, x+cw, gy+headH, x+cw, gridBottom, cCyan, 2)
			drawText(hdc, rect{x, gy, x + cw, gy + headH},
				fmt.Sprintf("%d", idx+1), cCyan, a.fonts.tiny, dtCenter)
		}
	}

	// 网格外框
	frameRect(hdc, a.lay.grid, cLine, 1)
}

// --------------------------------------------------------------------------
//  步进属性条（7 组：ON / ACC / VEL / PROB / MIC / RAT / COND）
// --------------------------------------------------------------------------
func (a *uiState) paintStepEditor(hdc uintptr) {
	L := a.lay
	fillRect(hdc, L.stepEd, cPanel)
	st := a.selStepPtr()

	drawText(hdc, mkRect(16, 492, 320, 16),
		fmt.Sprintf("步进  %s %02d   力度 %d", padCodes[a.selPad], a.selStep+1, st.Velocity),
		cOrange, a.fonts.bold, dtLeft)
	drawText(hdc, mkRect(L.stepEd.right-500, 492, 480, 16),
		"左键开关格子 · 右键弹出属性面板", cDim, a.fonts.tiny, dtRight)

	labels := [7]string{"ON 开关", "ACC 重音", "VEL 力度", "PROB 概率", "MIC 微时", "RAT 滚奏", "COND 条件"}
	vals := [7]string{
		boolText(st.On != 0, "ON", "OFF"),
		boolText(st.Accent != 0, "是", "否"),
		fmt.Sprintf("%d", st.Velocity),
		fmt.Sprintf("%d%%", st.Prob),
		fmt.Sprintf("%+d", st.Micro),
		fmt.Sprintf("x%d", st.Ratchet),
		condName(st.Cond),
	}

	for i := 0; i < 7; i++ {
		f := L.stepEdFields[i]
		drawText(hdc, mkRect(f.minus.left, 508, 180, 12), labels[i], cDim, a.fonts.tiny, dtLeft)
		a.button(hdc, f.minus, "-", false, true)
		a.button(hdc, f.plus, "+", false, true)
		a.stepValueBox(hdc, f.val, vals[i])
	}
}

// boolText 三元字符串。
func boolText(cond bool, yes, no string) string {
	if cond {
		return yes
	}
	return no
}

// --------------------------------------------------------------------------
//  当前声部参数
// --------------------------------------------------------------------------
func (a *uiState) paintParams(hdc uintptr) {
	L := a.lay
	fillRect(hdc, L.params, cPanel)
	p := &a.proj.Pads[a.selPad]

	drawText(hdc, mkRect(16, 574, 420, 18),
		fmt.Sprintf("PAD %02d   %s   %s", a.selPad, padCodes[a.selPad], padNamesCN[a.selPad]),
		cOrange, a.fonts.bold, dtLeft)

	// M / S 与轨长
	a.button(hdc, L.padMute, "M", atomic.LoadInt32(&p.Mute) != 0, true)
	a.button(hdc, L.padSolo, "S", atomic.LoadInt32(&p.Solo) != 0, true)
	drawText(hdc, mkRect(1230, 574, 44, 20), "轨长", cDim, a.fonts.tiny, dtLeft)
	a.button(hdc, L.lenMinus, "-", false, true)
	a.button(hdc, L.lenPlus, "+", false, true)
	pat := a.curPattern()
	a.stepValueBox(hdc, L.lenVal, fmt.Sprintf("%d", pat.TrackLen[a.selPad]))

	// 参数滑杆
	defs := padDefsByPad[a.selPad]
	for i, d := range defs {
		labelR, trackR := padSliderRect(i)
		v := *d.get(p)
		drawText(hdc, labelR, d.label, cDim, a.fonts.tiny, dtLeft)
		sliderTrack(hdc, trackR, v/d.max)
		drawText(hdc, rect{labelR.left, 632, labelR.right, 644},
			fmt.Sprintf("%d", int(v*100)), cOrange, a.fonts.tiny, dtLeft)
	}
}

// --------------------------------------------------------------------------
//  主效果 + 电平表
// --------------------------------------------------------------------------
func (a *uiState) paintMaster(hdc uintptr) {
	L := a.lay
	fillRect(hdc, L.master, cPanel)
	m := &a.proj.Master

	drawText(hdc, mkRect(16, 702, 380, 18), "MASTER · 徐某的母线", cOrange, a.fonts.bold, dtLeft)
	drawText(hdc, mkRect(360, 702, 420, 18),
		fmt.Sprintf("LP %s   HP %s", onOff(m.LpOn != 0), onOff(m.HpOn != 0)),
		cDim, a.fonts.tiny, dtLeft)

	// 5 个主滑杆
	for i, d := range masterDefs {
		labelR, trackR := masterSliderRect(i)
		v := *d.get(m)
		drawText(hdc, labelR, d.label, cDim, a.fonts.tiny, dtLeft)
		sliderTrack(hdc, trackR, (v-d.min)/(d.max-d.min))
		text := fmt.Sprintf("%d%s", int(v), d.unit)
		if d.unit == "" {
			text = fmt.Sprintf("%.2f", v)
		}
		drawText(hdc, rect{labelR.left, 760, labelR.right, 774}, text, cOrange, a.fonts.tiny, dtLeft)
	}

	// 电平表
	ml, mr := a.eng.Meters()
	drawText(hdc, mkRect(850, 722, 18, 14), "L", cDim, a.fonts.tiny, dtLeft)
	drawText(hdc, mkRect(850, 742, 18, 14), "R", cDim, a.fonts.tiny, dtLeft)
	for k, lvl := range []float64{ml, mr} {
		top := int32(724 + k*20)
		bar := mkRect(872, top, 380, 12)
		fillRect(hdc, bar, cTrack)
		w := int32(clampF(lvl, 0, 1) * float64(bar.w()-2))
		if w > 0 {
			col := cGreen
			if lvl > 0.92 {
				col = cRed
			} else if lvl > 0.75 {
				col = cYellow
			}
			fillRect(hdc, rect{bar.left + 1, bar.top + 1, bar.left + 1 + w, bar.bottom - 1}, col)
		}
		frameRect(hdc, bar, cLine, 1)
	}

	// 状态文字
	midiTxt := "-"
	if a.midiOK {
		midiTxt = "ON"
	}
	drawText(hdc, mkRect(872, 770, 600, 16),
		fmt.Sprintf("CPU %.1f%%    XRUN %d    MIDI %s    徐某 · Go 版",
			a.eng.CPU()*100, a.eng.xruns.Load(), midiTxt),
		cDim, a.fonts.tiny, dtLeft)

	// 简化的“主板”装饰：一条橙色短线
	line(hdc, 16, 690+36, 16, 690+66, cOrangeD, 2)
}

// onOff 中文开关文本。
func onOff(v bool) string {
	if v {
		return "ON"
	}
	return "OFF"
}

// --------------------------------------------------------------------------
//  底部状态与提示
// --------------------------------------------------------------------------
func (a *uiState) paintFooter(hdc uintptr) {
	fillRect(hdc, a.lay.footer, cBg)
	now, _, _ := pGetTickCount64.Call()
	status := "就绪 · 按空格开始播放"
	if a.status != "" && uint64(now) < a.statusUntil {
		status = a.status
	}
	drawText(hdc, mkRect(16, 830, 900, 18), status, cOrange, a.fonts.small, dtLeft)
	drawText(hdc, mkRect(16, 852, 1200, 18),
		"空格 播放/停止 · Z X C V B N M A S D F G H J K L 打鼓 · 1-8 切 Pattern · 关闭窗口会安全退出音频线程",
		cDim, a.fonts.tiny, dtLeft)
	drawText(hdc, mkRect(1100, 830, 384, 18), "徐某 · Go + Win32 + waveOut",
		cDim, a.fonts.tiny, dtRight)

	// 细微的装饰线
	line(hdc, 8, footerY, uiW-8, footerY, cLine, 1)
}
