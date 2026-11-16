// ==========================================================================
//  ui_overlay.go —— 三个弹层：步进属性 / 歌曲链 / 设置
//
//  署名：徐某。
//  用 GDI 手绘弹层而不是系统对话框，原因有三：
//    1. 视觉与主体 808 风格统一；
//    2. 不阻塞消息循环，播放时仍能听到声音；
//    3. 命中测试与绘制共用同一套矩形函数，不会出现“看得到点不到”。
// ==========================================================================
package main

import (
	"fmt"
	"sync/atomic"
)

// --------------------------------------------------------------------------
//  右键“步进属性”面板
// --------------------------------------------------------------------------
const (
	popW    = 400
	popRowH = 30
	popHead = 44
)

func (a *uiState) stepPopupLayout() (panel rect, closeR rect, rows [7]r3) {
	h := int32(popHead + 7*popRowH + 12)
	x := a.popupX
	y := a.popupY
	if x+popW > uiW-8 {
		x = uiW - 8 - popW
	}
	if x < 8 {
		x = 8
	}
	if y+h > uiH-8 {
		y = uiH - 8 - h
	}
	if y < 8 {
		y = 8
	}
	panel = mkRect(x, y, popW, h)
	closeR = mkRect(x+popW-34, y+8, 24, 24)
	for i := 0; i < 7; i++ {
		ry := y + popHead + int32(i)*popRowH
		rows[i] = r3{
			minus: mkRect(x+152, ry+4, 24, 22),
			val:   mkRect(x+180, ry+4, 148, 22),
			plus:  mkRect(x+332, ry+4, 24, 22),
		}
	}
	return
}

func (a *uiState) paintStepPopup(hdc uintptr) {
	panel, closeR, rows := a.stepPopupLayout()
	st := a.selStepPtr()

	// 阴影：向右下偏移 4px 的深色块，制造浮起感
	fillRect(hdc, rect{panel.left + 4, panel.top + 4, panel.right + 4, panel.bottom + 4}, rgb(8, 8, 8))
	fillRect(hdc, panel, cPanel2)
	frameRect(hdc, panel, cOrange, 1)

	drawText(hdc, mkRect(panel.left+12, panel.top+10, 240, 22),
		fmt.Sprintf("步进属性  %s %02d", padCodes[a.selPad], a.selStep+1),
		cOrange, a.fonts.bold, dtLeft)
	a.button(hdc, closeR, "×", false, true)

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
		f := rows[i]
		drawText(hdc, mkRect(panel.left+14, f.minus.top, 130, f.minus.h()), labels[i],
			cDim, a.fonts.tiny, dtLeft)
		a.button(hdc, f.minus, "-", false, true)
		a.button(hdc, f.plus, "+", false, true)
		a.stepValueBox(hdc, f.val, vals[i])
	}
}

// stepPopupClick 返回 true 表示点击已消费。
func (a *uiState) stepPopupClick(x, y int32) bool {
	panel, closeR, rows := a.stepPopupLayout()
	if closeR.contains(x, y) {
		a.overlay = ovNone
		return true
	}
	if !panel.contains(x, y) {
		a.overlay = ovNone // 点面板外部关闭
		return false
	}
	for i := 0; i < 7; i++ {
		f := rows[i]
		if f.minus.contains(x, y) {
			a.stepAdjust(i, -1)
			return true
		}
		if f.plus.contains(x, y) {
			a.stepAdjust(i, +1)
			return true
		}
		if f.val.contains(x, y) && (i == 0 || i == 1) {
			a.stepAdjust(i, +1)
			return true
		}
	}
	return true
}

// --------------------------------------------------------------------------
//  歌曲链编辑器
// --------------------------------------------------------------------------
const (
	songW = 660
	songH = 470
)

type songRow struct {
	patMinus, patVal, patPlus    rect
	repMinus, repVal, repPlus    rect
	del                          rect
}

func (a *uiState) songLayout() (panel rect, rows [8]songRow, add, prevPage, nextPage, closeR rect) {
	x := int32((uiW - songW) / 2)
	y := int32((uiH - songH) / 2)
	panel = mkRect(x, y, songW, songH)

	for i := 0; i < 8; i++ {
		ry := y + 96 + int32(i)*38
		rows[i] = songRow{
			patMinus: mkRect(x+170, ry+4, 24, 24),
			patVal:   mkRect(x+198, ry+4, 56, 24),
			patPlus:  mkRect(x+258, ry+4, 24, 24),
			repMinus: mkRect(x+350, ry+4, 24, 24),
			repVal:   mkRect(x+378, ry+4, 46, 24),
			repPlus:  mkRect(x+428, ry+4, 24, 24),
			del:      mkRect(x+520, ry+4, 44, 24),
		}
	}
	add = mkRect(x+16, y+songH-44, 90, 30)
	prevPage = mkRect(x+300, y+songH-44, 80, 30)
	nextPage = mkRect(x+388, y+songH-44, 80, 30)
	closeR = mkRect(x+songW-110, y+songH-44, 94, 30)
	return
}

func (a *uiState) paintSongEditor(hdc uintptr) {
	panel, rows, add, prevPage, nextPage, closeR := a.songLayout()
	fillRect(hdc, rect{panel.left + 5, panel.top + 5, panel.right + 5, panel.bottom + 5}, rgb(8, 8, 8))
	fillRect(hdc, panel, cPanel2)
	frameRect(hdc, panel, cOrange, 1)

	total := int(atomic.LoadInt32(&a.proj.SongCount))
	pages := (total + 7) / 8
	if pages < 1 {
		pages = 1
	}
	drawText(hdc, mkRect(panel.left+16, panel.top+12, 320, 24), "歌曲链  SONG CHAIN",
		cOrange, a.fonts.bold, dtLeft)
	drawText(hdc, mkRect(panel.left+350, panel.top+12, 290, 24),
		fmt.Sprintf("共 %d 节 · 第 %d/%d 页", total, a.songPage+1, pages),
		cDim, a.fonts.tiny, dtRight)

	// 表头
	drawText(hdc, mkRect(panel.left+20, panel.top+58, 60, 18), "序号", cDim, a.fonts.tiny, dtLeft)
	drawText(hdc, mkRect(panel.left+120, panel.top+58, 150, 18), "PATTERN", cDim, a.fonts.tiny, dtLeft)
	drawText(hdc, mkRect(panel.left+300, panel.top+58, 150, 18), "REPEATS", cDim, a.fonts.tiny, dtLeft)
	line(hdc, panel.left+16, panel.top+80, panel.right-16, panel.top+80, cLine, 1)

	for i := 0; i < 8; i++ {
		idx := a.songPage*8 + i
		if idx >= total {
			break
		}
		en := &a.proj.Song[idx]
		r := rows[i]
		drawText(hdc, mkRect(panel.left+20, r.del.top, 50, r.del.h()),
			fmt.Sprintf("%02d", idx+1), cDim, a.fonts.tiny, dtLeft)
		a.button(hdc, r.patMinus, "-", false, true)
		a.button(hdc, r.patPlus, "+", false, true)
		a.stepValueBox(hdc, r.patVal, fmt.Sprintf("P%02d", en.Pattern+1))
		a.button(hdc, r.repMinus, "-", false, true)
		a.button(hdc, r.repPlus, "+", false, true)
		a.stepValueBox(hdc, r.repVal, fmt.Sprintf("%d", en.Repeats))
		a.button(hdc, r.del, "删除", false, true)
	}

	a.button(hdc, add, "+ 添加", false, true)
	a.button(hdc, prevPage, "上一页", false, a.songPage > 0)
	a.button(hdc, nextPage, "下一页", false, a.songPage+1 < pages)
	a.button(hdc, closeR, "关闭", false, true)
}

func (a *uiState) songClick(x, y int32) bool {
	panel, rows, add, prevPage, nextPage, closeR := a.songLayout()
	if closeR.contains(x, y) {
		a.overlay = ovNone
		return true
	}
	if !panel.contains(x, y) {
		a.overlay = ovNone
		return false
	}
	total := int(atomic.LoadInt32(&a.proj.SongCount))
	pages := (total + 7) / 8
	if pages < 1 {
		pages = 1
	}
	if add.contains(x, y) {
		if total < 64 {
			a.proj.Song[total] = SongEntry{Pattern: atomic.LoadInt32(&a.proj.Current), Repeats: 4}
			atomic.StoreInt32(&a.proj.SongCount, int32(total+1))
			a.songPage = total / 8 // 跳到新增项所在页
		}
		return true
	}
	if prevPage.contains(x, y) {
		if a.songPage > 0 {
			a.songPage--
		}
		return true
	}
	if nextPage.contains(x, y) {
		if a.songPage+1 < pages {
			a.songPage++
		}
		return true
	}
	for i := 0; i < 8; i++ {
		idx := a.songPage*8 + i
		if idx >= total {
			break
		}
		r := rows[i]
		en := &a.proj.Song[idx]
		switch {
		case r.patMinus.contains(x, y):
			en.Pattern = (en.Pattern - 1 + Patterns) % Patterns
		case r.patPlus.contains(x, y):
			en.Pattern = (en.Pattern + 1) % Patterns
		case r.patVal.contains(x, y):
			// 点一下数值就把当前编辑中的 pattern 赋给它，方便快速编排
			en.Pattern = atomic.LoadInt32(&a.proj.Current)
		case r.repMinus.contains(x, y):
			if en.Repeats > 1 {
				en.Repeats--
			}
		case r.repPlus.contains(x, y):
			if en.Repeats < 64 {
				en.Repeats++
			}
		case r.del.contains(x, y):
			// 删除后整体前移，保持链紧凑
			for k := idx; k < total-1; k++ {
				a.proj.Song[k] = a.proj.Song[k+1]
			}
			atomic.StoreInt32(&a.proj.SongCount, int32(total-1))
			if a.songPage > 0 && a.songPage*8 >= total-1 {
				a.songPage--
			}
		}
	}
	return true
}

// --------------------------------------------------------------------------
//  设置面板：滤波开关 / 拍号 / MIDI 时钟
// --------------------------------------------------------------------------
const (
	setW = 480
	setH = 330
)

func (a *uiState) settingsLayout() (panel rect, lp, hp, beatMinus, beatVal, beatPlus, unit, clock, closeR rect) {
	x := int32((uiW - setW) / 2)
	y := int32((uiH - setH) / 2)
	panel = mkRect(x, y, setW, setH)
	lp = mkRect(x+250, y+62, 84, 28)
	hp = mkRect(x+250, y+102, 84, 28)
	beatMinus = mkRect(x+250, y+142, 26, 28)
	beatVal = mkRect(x+280, y+142, 54, 28)
	beatPlus = mkRect(x+338, y+142, 26, 28)
	unit = mkRect(x+250, y+182, 84, 28)
	clock = mkRect(x+250, y+222, 84, 28)
	closeR = mkRect(x+180, y+setH-44, 120, 30)
	return
}

func (a *uiState) paintSettings(hdc uintptr) {
	panel, lp, hp, beatMinus, beatVal, beatPlus, unit, clock, closeR := a.settingsLayout()
	fillRect(hdc, rect{panel.left + 5, panel.top + 5, panel.right + 5, panel.bottom + 5}, rgb(8, 8, 8))
	fillRect(hdc, panel, cPanel2)
	frameRect(hdc, panel, cOrange, 1)

	drawText(hdc, mkRect(panel.left+16, panel.top+12, 300, 22), "设置  SETTINGS",
		cOrange, a.fonts.bold, dtLeft)

	m := &a.proj.Master
	label := func(r rect, s string) {
		drawText(hdc, mkRect(panel.left+24, r.top, 200, r.h()), s, cDim, a.fonts.tiny, dtLeft)
	}
	label(lp, "低通滤波 LP")
	a.button(hdc, lp, onOff(m.LpOn != 0), m.LpOn != 0, true)
	label(hp, "高通滤波 HP")
	a.button(hdc, hp, onOff(m.HpOn != 0), m.HpOn != 0, true)
	label(beatMinus, "每小节拍数")
	a.button(hdc, beatMinus, "-", false, true)
	a.button(hdc, beatPlus, "+", false, true)
	a.stepValueBox(hdc, beatVal, fmt.Sprintf("%d", m.BeatsPerBar))
	label(unit, "拍号单位")
	a.button(hdc, unit, fmt.Sprintf("/%d", m.BeatUnit), false, true)
	label(clock, "MIDI 外部时钟")
	a.button(hdc, clock, onOff(a.eng.clockExternal.Load() != 0), a.eng.clockExternal.Load() != 0, true)

	drawText(hdc, mkRect(panel.left+24, panel.top+262, 420, 16),
		fmt.Sprintf("采样率 %d Hz · 块 %d · 16 voice · XU-808 Go", XUSR, BlockSize),
		cDim, a.fonts.tiny, dtLeft)

	a.button(hdc, closeR, "关闭", false, true)
}

func (a *uiState) settingsClick(x, y int32) bool {
	panel, lp, hp, beatMinus, beatVal, beatPlus, unit, clock, closeR := a.settingsLayout()
	if closeR.contains(x, y) {
		a.overlay = ovNone
		return true
	}
	if !panel.contains(x, y) {
		a.overlay = ovNone
		return false
	}
	m := &a.proj.Master
	switch {
	case lp.contains(x, y):
		m.LpOn = 1 - m.LpOn
	case hp.contains(x, y):
		m.HpOn = 1 - m.HpOn
	case beatMinus.contains(x, y):
		if m.BeatsPerBar > 1 {
			m.BeatsPerBar--
		}
	case beatPlus.contains(x, y):
		if m.BeatsPerBar < 16 {
			m.BeatsPerBar++
		}
	case beatVal.contains(x, y):
		m.BeatsPerBar = 4
	case unit.contains(x, y):
		if m.BeatUnit == 4 {
			m.BeatUnit = 8
		} else {
			m.BeatUnit = 4
		}
	case clock.contains(x, y):
		a.eng.clockExternal.Store(1 - a.eng.clockExternal.Load())
	}
	return true
}

// overlayClick 弹层总入口。
func (a *uiState) overlayClick(x, y int32) {
	switch a.overlay {
	case ovStep:
		a.stepPopupClick(x, y)
	case ovSong:
		a.songClick(x, y)
	case ovSet:
		a.settingsClick(x, y)
	}
}
