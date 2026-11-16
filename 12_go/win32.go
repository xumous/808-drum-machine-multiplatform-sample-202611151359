// ==========================================================================
//  win32.go —— 纯 syscall 的 Win32 / GDI / waveOut / winmm / comdlg32 绑定
//
//  署名：徐某。
//  不使用 cgo，也不引入任何第三方 GUI 库：只用 syscall.NewLazyDLL 拿函数地址，
//  unsafe.Pointer 传结构体指针，所有结构体布局都按 x64 ABI 手工对齐。
//  这样 go build 出来就是单个 EXE，CGO_ENABLED=0 也能过。
// ==========================================================================
package main

import (
	"runtime"
	"syscall"
	"unsafe"
)

// ---- 五个系统 DLL ----
var (
	user32   = syscall.NewLazyDLL("user32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	winmm    = syscall.NewLazyDLL("winmm.dll")
	comdlg32 = syscall.NewLazyDLL("comdlg32.dll")
)

// ---- user32 ----
var (
	pRegisterClassExW = user32.NewProc("RegisterClassExW")
	pCreateWindowExW  = user32.NewProc("CreateWindowExW")
	pDefWindowProcW   = user32.NewProc("DefWindowProcW")
	pDestroyWindow    = user32.NewProc("DestroyWindow")
	pShowWindow       = user32.NewProc("ShowWindow")
	pUpdateWindow     = user32.NewProc("UpdateWindow")
	pGetMessageW      = user32.NewProc("GetMessageW")
	pTranslateMessage = user32.NewProc("TranslateMessage")
	pDispatchMessageW = user32.NewProc("DispatchMessageW")
	pPostQuitMessage  = user32.NewProc("PostQuitMessage")
	pInvalidateRect   = user32.NewProc("InvalidateRect")
	pGetClientRect    = user32.NewProc("GetClientRect")
	pBeginPaint       = user32.NewProc("BeginPaint")
	pEndPaint         = user32.NewProc("EndPaint")
	pSetTimer         = user32.NewProc("SetTimer")
	pKillTimer        = user32.NewProc("KillTimer")
	pLoadCursorW      = user32.NewProc("LoadCursorW")
	pLoadIconW        = user32.NewProc("LoadIconW")
	pSetCursor        = user32.NewProc("SetCursor")
	pSetCapture       = user32.NewProc("SetCapture")
	pReleaseCapture   = user32.NewProc("ReleaseCapture")
	pMessageBoxW      = user32.NewProc("MessageBoxW")
	pDrawTextW        = user32.NewProc("DrawTextW")
	pFillRect         = user32.NewProc("FillRect")
	pGetSystemMetrics = user32.NewProc("GetSystemMetrics")
	pAdjustWindowRect = user32.NewProc("AdjustWindowRect")
	pSetProcessDPIAware = user32.NewProc("SetProcessDPIAware")
	pSetWindowTextW   = user32.NewProc("SetWindowTextW")
	pScreenToClient   = user32.NewProc("ScreenToClient")
)

// ---- gdi32 ----
var (
	pCreateCompatibleDC     = gdi32.NewProc("CreateCompatibleDC")
	pCreateCompatibleBitmap = gdi32.NewProc("CreateCompatibleBitmap")
	pSelectObject           = gdi32.NewProc("SelectObject")
	pDeleteObject           = gdi32.NewProc("DeleteObject")
	pDeleteDC               = gdi32.NewProc("DeleteDC")
	pBitBlt                 = gdi32.NewProc("BitBlt")
	pCreateSolidBrush       = gdi32.NewProc("CreateSolidBrush")
	pCreatePen              = gdi32.NewProc("CreatePen")
	pRectangle              = gdi32.NewProc("Rectangle")
	pMoveToEx               = gdi32.NewProc("MoveToEx")
	pLineTo                 = gdi32.NewProc("LineTo")
	pSetTextColor           = gdi32.NewProc("SetTextColor")
	pSetBkMode              = gdi32.NewProc("SetBkMode")
	pCreateFontW            = gdi32.NewProc("CreateFontW")
	pGetStockObject         = gdi32.NewProc("GetStockObject")
	pStretchBlt             = gdi32.NewProc("StretchBlt")
	pSetStretchBltMode      = gdi32.NewProc("SetStretchBltMode")
)

// ---- kernel32 ----
var (
	pGetModuleHandleW          = kernel32.NewProc("GetModuleHandleW")
	pGetTickCount64            = kernel32.NewProc("GetTickCount64")
	pQueryPerformanceCounter   = kernel32.NewProc("QueryPerformanceCounter")
	pQueryPerformanceFrequency = kernel32.NewProc("QueryPerformanceFrequency")
	pCreateEventW              = kernel32.NewProc("CreateEventW")
	pWaitForSingleObject       = kernel32.NewProc("WaitForSingleObject")
	pCloseHandle               = kernel32.NewProc("CloseHandle")
	pSleep                     = kernel32.NewProc("Sleep")
	pMoveFileExW               = kernel32.NewProc("MoveFileExW")
)

// ---- winmm ----
var (
	pWaveOutGetNumDevs        = winmm.NewProc("waveOutGetNumDevs")
	pWaveOutOpen              = winmm.NewProc("waveOutOpen")
	pWaveOutPrepareHeader     = winmm.NewProc("waveOutPrepareHeader")
	pWaveOutWrite             = winmm.NewProc("waveOutWrite")
	pWaveOutReset             = winmm.NewProc("waveOutReset")
	pWaveOutUnprepareHeader   = winmm.NewProc("waveOutUnprepareHeader")
	pWaveOutClose             = winmm.NewProc("waveOutClose")
	pMidiInGetNumDevs         = winmm.NewProc("midiInGetNumDevs")
	pMidiInOpen               = winmm.NewProc("midiInOpen")
	pMidiInStart              = winmm.NewProc("midiInStart")
	pMidiInStop               = winmm.NewProc("midiInStop")
	pMidiInReset              = winmm.NewProc("midiInReset")
	pMidiInClose              = winmm.NewProc("midiInClose")
)

// ---- comdlg32 ----
var (
	pGetSaveFileNameW = comdlg32.NewProc("GetSaveFileNameW")
	pGetOpenFileNameW = comdlg32.NewProc("GetOpenFileNameW")
)

// ---- 常用常量（与 Windows SDK 一致，避免引入头文件） ----
const (
	// 窗口样式
	wsOverlappedWindow = 0x00CF0000
	wsCaption          = 0x00C00000
	wsSysMenu          = 0x00080000
	wsThickFrame       = 0x00040000
	wsMinimizeBox      = 0x00020000
	wsMaximizeBox      = 0x00010000
	// 消息
	wmCreate     = 0x0001
	wmDestroy    = 0x0002
	wmSize       = 0x0005
	wmPaint      = 0x000F
	wmClose      = 0x0010
	wmEraseBkgnd = 0x0014
	wmTimer      = 0x0113
	wmKeyDown    = 0x0100
	wmMouseMove  = 0x0200
	wmLButtonDown = 0x0201
	wmLButtonUp   = 0x0202
	wmRButtonDown = 0x0204
	wmRButtonUp   = 0x0205
	wmMouseWheel  = 0x020A
	// 显示
	swShow = 5
	// 光标
	idcArrow = 32512
	// GDI
	psSolid      = 0
	nullBrush    = 5
	transparent  = 1
	srcCopy      = 0x00CC0020
	dtLeft       = 0x0000
	dtCenter     = 0x0001
	dtRight      = 0x0002
	dtVCenter    = 0x0004
	dtSingleLine = 0x0020
	// 字体
	fwNormal = 400
	fwBold   = 700
	// 对话框
	ofnOverwritePrompt = 0x00000002
	ofnPathMustExist   = 0x00000800
	ofnFileMustExist   = 0x00001000
	ofnHideReadOnly    = 0x00000004
	// MMSYS
	waveMapper     = 0xFFFFFFFF
	callbackEvent  = 0x00050000
	callbackFunc   = 0x00030000
	mmSysErrNoError = 0
	// 虚拟键
	vkSpace   = 0x20
	vkEscape  = 0x1B
	vkReturn  = 0x0D
	vkLeft    = 0x25
	vkRight   = 0x27
	vkUp      = 0x26
	vkDown    = 0x28
	vkDelete  = 0x2E
	// MoveFileEx 标志
	moveFileReplaceExisting = 0x00000001
	moveFileWriteThrough    = 0x00000008
)

// ---- 坐标结构 ----
type point struct{ x, y int32 }

type rect struct {
	left, top, right, bottom int32
}

// r 便捷构造：用 x/y/w/h 表达一个矩形。
func mkRect(x, y, w, h int32) rect {
	return rect{left: x, top: y, right: x + w, bottom: y + h}
}

// w/h 取宽高，避免到处写 right-left。
func (r rect) w() int32 { return r.right - r.left }
func (r rect) h() int32 { return r.bottom - r.top }

// contains 判断点是否落在矩形内（右下开区间，符合 Win32 习惯）。
func (r rect) contains(x, y int32) bool {
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom
}

// WNDCLASSEXW（x64 布局：80 字节）
type wndClassExW struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

// MSG（x64 布局：48 字节）
type msgT struct {
	hwnd     uintptr
	message  uint32
	_        uint32
	wParam   uintptr
	lParam   uintptr
	time     uint32
	ptX      int32
	ptY      int32
	lPrivate uint32
}

// PAINTSTRUCT（x64 布局：72 字节）
type paintStruct struct {
	hdc         uintptr
	fErase      int32
	rcPaint     rect
	fRestore    int32
	fIncUpdate  int32
	rgbReserved [32]byte
}

// WAVEFORMATEX（18 字节；Go 结构体会补齐到 20，但 Win32 只读前 18 字节）
type waveFormatEx struct {
	wFormatTag      uint16
	nChannels       uint16
	nSamplesPerSec  uint32
	nAvgBytesPerSec uint32
	nBlockAlign     uint16
	wBitsPerSample  uint16
	cbSize          uint16
}

// WAVEHDR（x64 布局：48 字节）
type waveHdr struct {
	lpData          uintptr
	dwBufferLength  uint32
	dwBytesRecorded uint32
	dwUser          uintptr
	dwFlags         uint32
	dwLoops         uint32
	lpNext          uintptr
	reserved        uintptr
}

// OPENFILENAMEW（x64 布局：152 字节），字段间的 _pad 是为了让偏移与 SDK 一致。
type openFileNameW struct {
	lStructSize       uint32
	_pad0             uint32
	hwndOwner         uintptr
	hInstance         uintptr
	lpstrFilter       *uint16
	lpstrCustomFilter *uint16
	nMaxCustFilter    uint32
	nFilterIndex      uint32
	lpstrFile         *uint16
	nMaxFile          uint32
	_pad1             uint32
	lpstrFileTitle    *uint16
	nMaxFileTitle     uint32
	_pad2             uint32
	lpstrInitialDir   *uint16
	lpstrTitle        *uint16
	flags             uint32
	nFileOffset       uint16
	nFileExtension    uint16
	lpstrDefExt       *uint16
	lCustData         uintptr
	lpfnHook          uintptr
	lpTemplateName    *uint16
	pvReserved        uintptr
	dwReserved        uint32
	flagsEx           uint32
}

// --------------------------------------------------------------------------
//  字符串与 GDI 小工具
// --------------------------------------------------------------------------

// wstr 把 Go 字符串转成 NUL 结尾的 UTF-16 指针。
func wstr(s string) *uint16 {
	p, err := syscall.UTF16PtrFromString(s)
	if err != nil {
		// 出现非法字符（理论上不会有）时退化成空串，绝不 panic 到消息循环
		p, _ = syscall.UTF16PtrFromString("")
	}
	return p
}

// utf16Filter 构造 Win32 文件对话框要求的双 NUL 结尾过滤器。
func utf16Filter(parts ...string) []uint16 {
	out := make([]uint16, 0, 128)
	for _, s := range parts {
		u, _ := syscall.UTF16FromString(s)
		out = append(out, u...) // UTF16FromString 自带结尾 0
	}
	out = append(out, 0) // 整体再补一个 0
	return out
}

// rgb 把 0xRRGGBB 转成 Win32 的 COLORREF（0x00BBGGRR）。
func rgb(r, g, b uint32) uintptr {
	return uintptr(r&0xFF | (g&0xFF)<<8 | (b&0xFF)<<16)
}

// 画刷/画笔缓存：颜色种类很少，缓存后每帧只需极少 GDI 对象创建。
var (
	brushCache = map[uintptr]uintptr{}
	penCache   = map[[2]uintptr]uintptr{}
)

func solidBrush(color uintptr) uintptr {
	if b, ok := brushCache[color]; ok {
		return b
	}
	b, _, _ := pCreateSolidBrush.Call(color)
	brushCache[color] = b
	return b
}

func solidPen(color uintptr, width int) uintptr {
	key := [2]uintptr{color, uintptr(width)}
	if p, ok := penCache[key]; ok {
		return p
	}
	p, _, _ := pCreatePen.Call(psSolid, uintptr(width), color)
	penCache[key] = p
	return p
}

// fillRect 纯色填充。
func fillRect(hdc uintptr, r rect, color uintptr) {
	pFillRect.Call(hdc, uintptr(unsafe.Pointer(&r)), solidBrush(color))
}

// frameRect 只画边框（内部透明）。
func frameRect(hdc uintptr, r rect, color uintptr, width int) {
	pen := solidPen(color, width)
	oldPen, _, _ := pSelectObject.Call(hdc, pen)
	oldBrush, _, _ := pSelectObject.Call(hdc, stockNullBrush())
	pRectangle.Call(hdc, uintptr(r.left), uintptr(r.top), uintptr(r.right), uintptr(r.bottom))
	// Rectangle 的右/下边界是开区间，需要再补一像素才能得到视觉上的 w×h
	pSelectObject.Call(hdc, oldBrush)
	pSelectObject.Call(hdc, oldPen)
}

func stockNullBrush() uintptr {
	h, _, _ := pGetStockObject.Call(nullBrush)
	return h
}

// drawText 在矩形内绘制一行文字，align 用 DT_* 常量组合。
func drawText(hdc uintptr, r rect, s string, color uintptr, font uintptr, align uint32) {
	pSelectObject.Call(hdc, font)
	pSetTextColor.Call(hdc, color)
	pSetBkMode.Call(hdc, transparent)
	p := wstr(s)
	pDrawTextW.Call(hdc, uintptr(unsafe.Pointer(p)), ^uintptr(0), uintptr(unsafe.Pointer(&r)),
		uintptr(align|dtSingleLine|dtVCenter))
	runtime.KeepAlive(p)
}

// line 画一条直线。
func line(hdc uintptr, x1, y1, x2, y2 int32, color uintptr, width int) {
	pen := solidPen(color, width)
	old, _, _ := pSelectObject.Call(hdc, pen)
	pMoveToEx.Call(hdc, uintptr(x1), uintptr(y1), 0)
	pLineTo.Call(hdc, uintptr(x2), uintptr(y2))
	pSelectObject.Call(hdc, old)
}

// GET_X_LPARAM / GET_Y_LPARAM：鼠标坐标在 lParam 的低/高 16 位（有符号）。
func getXLP(lp uintptr) int32 { return int32(int16(lp & 0xFFFF)) }
func getYLP(lp uintptr) int32 { return int32(int16((lp >> 16) & 0xFFFF)) }

// --------------------------------------------------------------------------
//  文件对话框：成功返回完整路径
// --------------------------------------------------------------------------

// moveFileReplace 用 MoveFileExW 覆盖式改名（MOVEFILE_REPLACE_EXISTING）。
// 这是 Windows 上“原子替换已有文件”的正确做法：目标不存在则直接创建，
// 目标存在则覆盖，不会像裸 rename 那样在部分环境下因目标被占用而失败。
func moveFileReplace(from, to string) error {
	pFrom := wstr(from)
	pTo := wstr(to)
	r, _, lastErr := pMoveFileExW.Call(
		uintptr(unsafe.Pointer(pFrom)),
		uintptr(unsafe.Pointer(pTo)),
		moveFileReplaceExisting|moveFileWriteThrough,
	)
	runtime.KeepAlive(pFrom)
	runtime.KeepAlive(pTo)
	if r != 0 {
		return nil
	}
	if lastErr != nil && lastErr != syscall.Errno(0) {
		return lastErr
	}
	return syscall.EINVAL
}
func saveFileDialog(owner uintptr, defaultName string, filter []uint16, defExt string) (string, bool) {
	buf := make([]uint16, 1024)
	u, _ := syscall.UTF16FromString(defaultName)
	copy(buf, u)
	ofn := openFileNameW{
		lStructSize: uint32(unsafe.Sizeof(openFileNameW{})),
		hwndOwner:   owner,
		lpstrFilter: &filter[0],
		lpstrFile:   &buf[0],
		nMaxFile:    1024,
		lpstrDefExt: wstr(defExt),
		flags:       ofnOverwritePrompt | ofnPathMustExist | ofnHideReadOnly,
	}
	r, _, _ := pGetSaveFileNameW.Call(uintptr(unsafe.Pointer(&ofn)))
	runtime.KeepAlive(buf)
	runtime.KeepAlive(filter)
	if r == 0 {
		return "", false
	}
	return syscall.UTF16ToString(buf), true
}

func openFileDialog(owner uintptr, defaultName string, filter []uint16, defExt string) (string, bool) {
	buf := make([]uint16, 1024)
	u, _ := syscall.UTF16FromString(defaultName)
	copy(buf, u)
	ofn := openFileNameW{
		lStructSize: uint32(unsafe.Sizeof(openFileNameW{})),
		hwndOwner:   owner,
		lpstrFilter: &filter[0],
		lpstrFile:   &buf[0],
		nMaxFile:    1024,
		lpstrDefExt: wstr(defExt),
		flags:       ofnFileMustExist | ofnPathMustExist | ofnHideReadOnly,
	}
	r, _, _ := pGetOpenFileNameW.Call(uintptr(unsafe.Pointer(&ofn)))
	runtime.KeepAlive(buf)
	runtime.KeepAlive(filter)
	if r == 0 {
		return "", false
	}
	return syscall.UTF16ToString(buf), true
}
