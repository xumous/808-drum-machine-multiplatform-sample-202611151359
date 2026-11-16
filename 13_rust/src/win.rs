// ============================================================================
//  win.rs —— 手写 Win32 FFI（作者：徐某）
//
//  为什么不用 windows-sys / winapi crate？
//    1. 规格要求“零 crates、离线可构建”，手写最可靠；
//    2. 我们只用到约 60 个函数，全部列在下面，一眼能看全；
//    3. 结构体布局照着 Windows SDK 头文件抄，ABI 一致。
//
//  所有类型别名都按 Win64（LLP64）定义：指针 8 字节、LONG 4 字节。
// ============================================================================
#![allow(non_snake_case, non_camel_case_types, dead_code)]

use std::ffi::c_void;

// ------------------------------ 基础类型 ------------------------------
pub type BOOL = i32;
pub type BYTE = u8;
pub type WORD = u16;
pub type DWORD = u32;
pub type LONG = i32;
pub type UINT = u32;
pub type WPARAM = usize;
pub type LPARAM = isize;
pub type LRESULT = isize;
pub type DWORD_PTR = usize;
pub type UINT_PTR = usize;
pub type ATOM = u16;
pub type COLORREF = u32;

pub type HWND = *mut c_void;
pub type HINSTANCE = *mut c_void;
pub type HICON = *mut c_void;
pub type HCURSOR = *mut c_void;
pub type HBRUSH = *mut c_void;
pub type HFONT = *mut c_void;
pub type HPEN = *mut c_void;
pub type HDC = *mut c_void;
pub type HBITMAP = *mut c_void;
pub type HGDIOBJ = *mut c_void;
pub type HMENU = *mut c_void;
pub type HMIDIIN = *mut c_void;
pub type HWAVEOUT = *mut c_void;
pub type HANDLE = *mut c_void;

/// 窗口消息处理函数指针（C 的 WNDPROC）
pub type WNDPROC = Option<unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT>;
/// waveOut / midiIn 的回调函数指针（C 的 CALLBACK）
pub type WAVECALLBACK = Option<unsafe extern "system" fn(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR)>;
pub type MIDIINPROC = Option<unsafe extern "system" fn(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR)>;

// ------------------------------ 结构体 ------------------------------
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RECT {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl RECT {
    /// 常用的小构造器：SetRect 的 Rust 版
    pub fn new(l: i32, t: i32, r: i32, b: i32) -> RECT {
        RECT { left: l, top: t, right: r, bottom: b }
    }
    /// 命中测试（右下开区间，和 Win32 的惯例一致）
    pub fn inside(&self, x: i32, y: i32) -> bool {
        x >= self.left && x < self.right && y >= self.top && y < self.bottom
    }
    pub fn w(&self) -> i32 {
        self.right - self.left
    }
    pub fn h(&self) -> i32 {
        self.bottom - self.top
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct POINT {
    pub x: i32,
    pub y: i32,
}

/// 窗口类（WNDCLASSW）。字段顺序必须与 windows.h 完全一致。
#[repr(C)]
pub struct WNDCLASSW {
    pub style: UINT,
    pub lpfnWndProc: WNDPROC,
    pub cbClsExtra: i32,
    pub cbWndExtra: i32,
    pub hInstance: HINSTANCE,
    pub hIcon: HICON,
    pub hCursor: HCURSOR,
    pub hbrBackground: HBRUSH,
    pub lpszMenuName: *const u16,
    pub lpszClassName: *const u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PAINTSTRUCT {
    pub hdc: HDC,
    pub fErase: BOOL,
    pub rcPaint: RECT,
    pub fRestore: BOOL,
    pub fIncUpdate: BOOL,
    pub rgbReserved: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MSG {
    pub hwnd: HWND,
    pub message: UINT,
    pub wParam: WPARAM,
    pub lParam: LPARAM,
    pub time: DWORD,
    pub pt: POINT,
}

/// 波形格式（WAVEFORMATEX）：16bit / 48kHz / 立体声
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WAVEFORMATEX {
    pub wFormatTag: WORD,
    pub nChannels: WORD,
    pub nSamplesPerSec: DWORD,
    pub nAvgBytesPerSec: DWORD,
    pub nBlockAlign: WORD,
    pub wBitsPerSample: WORD,
    pub cbSize: WORD,
}

/// 音频块头（WAVEHDR）
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WAVEHDR {
    pub lpData: *mut u8,
    pub dwBufferLength: DWORD,
    pub dwBytesRecorded: DWORD,
    pub dwUser: DWORD_PTR,
    pub dwFlags: DWORD,
    pub dwLoops: DWORD,
    pub lpNext: *mut WAVEHDR,
    pub reserved: DWORD_PTR,
}

impl Default for WAVEHDR {
    fn default() -> Self {
        WAVEHDR {
            lpData: std::ptr::null_mut(),
            dwBufferLength: 0,
            dwBytesRecorded: 0,
            dwUser: 0,
            dwFlags: 0,
            dwLoops: 0,
            lpNext: std::ptr::null_mut(),
            reserved: 0,
        }
    }
}

/// 打开/保存文件对话框（OPENFILENAMEW）
#[repr(C)]
pub struct OPENFILENAMEW {
    pub lStructSize: DWORD,
    pub hwndOwner: HWND,
    pub hInstance: HINSTANCE,
    pub lpstrFilter: *const u16,
    pub lpstrCustomFilter: *mut u16,
    pub nMaxCustFilter: DWORD,
    pub nFilterIndex: DWORD,
    pub lpstrFile: *mut u16,
    pub nMaxFile: DWORD,
    pub lpstrFileTitle: *mut u16,
    pub nMaxFileTitle: DWORD,
    pub lpstrInitialDir: *const u16,
    pub lpstrTitle: *const u16,
    pub Flags: DWORD,
    pub nFileOffset: WORD,
    pub nFileExtension: WORD,
    pub lpstrDefExt: *const u16,
    pub lCustData: LPARAM,
    pub lpfnHook: *mut c_void,
    pub lpTemplateName: *const u16,
    pub pvReserved: *mut c_void,
    pub dwReserved: DWORD,
    pub FlagsEx: DWORD,
}

impl Default for OPENFILENAMEW {
    fn default() -> Self {
        // 全零对 OPENFILENAMEW 是合法初始值，唯一必须填的是 lStructSize。
        unsafe { std::mem::zeroed() }
    }
}

// ------------------------------ 常量 ------------------------------
pub const WS_OVERLAPPEDWINDOW: DWORD = 0x00CF_0000;
pub const WS_MAXIMIZEBOX: DWORD = 0x0001_0000;
pub const WS_POPUP: DWORD = 0x8000_0000;
pub const WS_CAPTION: DWORD = 0x00C0_0000;
pub const WS_SYSMENU: DWORD = 0x0008_0000;
pub const CW_USEDEFAULT: i32 = 0x8000_0000u32 as i32;
pub const SW_SHOW: i32 = 5;

pub const WM_CREATE: UINT = 0x0001;
pub const WM_DESTROY: UINT = 0x0002;
pub const WM_SIZE: UINT = 0x0005;
pub const WM_PAINT: UINT = 0x000F;
pub const WM_CLOSE: UINT = 0x0010;
pub const WM_KEYDOWN: UINT = 0x0100;
pub const WM_TIMER: UINT = 0x0113;
pub const WM_LBUTTONDOWN: UINT = 0x0201;
pub const WM_LBUTTONUP: UINT = 0x0202;
pub const WM_RBUTTONDOWN: UINT = 0x0204;
pub const WM_MOUSEMOVE: UINT = 0x0200;

pub const VK_SPACE: usize = 0x20;
pub const VK_SHIFT: i32 = 0x10;
pub const VK_CONTROL: i32 = 0x11;

pub const DT_LEFT: UINT = 0x0000;
pub const DT_CENTER: UINT = 0x0001;
pub const DT_RIGHT: UINT = 0x0002;
pub const DT_VCENTER: UINT = 0x0004;
pub const DT_SINGLELINE: UINT = 0x0020;

pub const TRANSPARENT: i32 = 1;
pub const SRCCOPY: DWORD = 0x00CC_0020;

pub const BLACK_BRUSH: i32 = 4;
pub const NULL_BRUSH: i32 = 5;

pub const PS_SOLID: i32 = 0;

pub const IDC_ARROW: usize = 32512;
pub const IDC_WAIT: usize = 32514;

pub const DEFAULT_CHARSET: DWORD = 1;
pub const OUT_DEFAULT_PRECIS: DWORD = 0;
pub const CLIP_DEFAULT_PRECIS: DWORD = 0;
pub const CLEARTYPE_QUALITY: DWORD = 5;
pub const DEFAULT_PITCH: DWORD = 0;
pub const FW_NORMAL: i32 = 400;
pub const FW_BOLD: i32 = 700;

pub const MB_OK: UINT = 0x0;
pub const MB_ICONERROR: UINT = 0x10;

pub const OFN_OVERWRITEPROMPT: DWORD = 0x0000_0002;
pub const OFN_PATHMUSTEXIST: DWORD = 0x0000_0800;
pub const OFN_FILEMUSTEXIST: DWORD = 0x0000_1000;
pub const MAX_PATH: usize = 260;

// waveOut / midiIn
pub const WAVE_MAPPER: UINT = 0xFFFF_FFFF;
pub const WAVE_FORMAT_PCM: WORD = 1;
pub const WOM_DONE: UINT = 0x03BD;
pub const CALLBACK_FUNCTION: DWORD = 0x0003_0000;
pub const MMSYSERR_NOERROR: u32 = 0;
pub const MIM_DATA: UINT = 0x03C3;

// ------------------------------ user32 ------------------------------
#[link(name = "user32")]
extern "system" {
    pub fn RegisterClassW(lpWndClass: *const WNDCLASSW) -> ATOM;
    pub fn CreateWindowExW(
        dwExStyle: DWORD,
        lpClassName: *const u16,
        lpWindowName: *const u16,
        dwStyle: DWORD,
        X: i32,
        Y: i32,
        nWidth: i32,
        nHeight: i32,
        hWndParent: HWND,
        hMenu: HMENU,
        hInstance: HINSTANCE,
        lpParam: *mut c_void,
    ) -> HWND;
    pub fn DefWindowProcW(hWnd: HWND, Msg: UINT, wParam: WPARAM, lParam: LPARAM) -> LRESULT;
    pub fn ShowWindow(hWnd: HWND, nCmdShow: i32) -> BOOL;
    pub fn UpdateWindow(hWnd: HWND) -> BOOL;
    pub fn DestroyWindow(hWnd: HWND) -> BOOL;
    pub fn GetMessageW(lpMsg: *mut MSG, hWnd: HWND, wMsgFilterMin: UINT, wMsgFilterMax: UINT) -> BOOL;
    pub fn TranslateMessage(lpMsg: *const MSG) -> BOOL;
    pub fn DispatchMessageW(lpMsg: *const MSG) -> LRESULT;
    pub fn PostQuitMessage(nExitCode: i32);
    pub fn InvalidateRect(hWnd: HWND, lpRect: *const RECT, bErase: BOOL) -> BOOL;
    pub fn GetClientRect(hWnd: HWND, lpRect: *mut RECT) -> BOOL;
    pub fn GetWindowRect(hWnd: HWND, lpRect: *mut RECT) -> BOOL;
    pub fn BeginPaint(hWnd: HWND, lpPaint: *mut PAINTSTRUCT) -> HDC;
    pub fn EndPaint(hWnd: HWND, lpPaint: *const PAINTSTRUCT) -> BOOL;
    pub fn SetTimer(hWnd: HWND, nIDEvent: UINT_PTR, uElapse: UINT, lpTimerFunc: *mut c_void) -> UINT_PTR;
    pub fn KillTimer(hWnd: HWND, uIDEvent: UINT_PTR) -> BOOL;
    pub fn SetCapture(hWnd: HWND) -> HWND;
    pub fn ReleaseCapture() -> BOOL;
    pub fn SetCursor(hCursor: HCURSOR) -> HCURSOR;
    pub fn LoadCursorW(hInstance: HINSTANCE, lpCursorName: *const u16) -> HCURSOR;
    pub fn LoadIconW(hInstance: HINSTANCE, lpIconName: *const u16) -> HICON;
    pub fn AdjustWindowRect(lpRect: *mut RECT, dwStyle: DWORD, bMenu: BOOL) -> BOOL;
    pub fn MessageBoxW(hWnd: HWND, lpText: *const u16, lpCaption: *const u16, uType: UINT) -> i32;
    pub fn SetWindowTextW(hWnd: HWND, lpString: *const u16) -> BOOL;
    pub fn GetSystemMetrics(nIndex: i32) -> i32;
    pub fn GetKeyState(nVirtKey: i32) -> i16;
    pub fn SetProcessDPIAware() -> BOOL;
}

// ------------------------------ gdi32 ------------------------------
#[link(name = "gdi32")]
extern "system" {
    pub fn CreateCompatibleDC(hdc: HDC) -> HDC;
    pub fn CreateCompatibleBitmap(hdc: HDC, cx: i32, cy: i32) -> HBITMAP;
    pub fn SelectObject(hdc: HDC, h: HGDIOBJ) -> HGDIOBJ;
    pub fn DeleteObject(o: HGDIOBJ) -> BOOL;
    pub fn DeleteDC(hdc: HDC) -> BOOL;
    pub fn BitBlt(dst: HDC, x: i32, y: i32, w: i32, h: i32, src: HDC, sx: i32, sy: i32, rop: DWORD) -> BOOL;
    pub fn CreateSolidBrush(c: COLORREF) -> HBRUSH;
    pub fn CreatePen(style: i32, width: i32, c: COLORREF) -> HPEN;
    pub fn FillRect(hdc: HDC, r: *const RECT, b: HBRUSH) -> i32;
    pub fn Rectangle(hdc: HDC, l: i32, t: i32, r: i32, b: i32) -> BOOL;
    pub fn MoveToEx(hdc: HDC, x: i32, y: i32, p: *mut POINT) -> BOOL;
    pub fn LineTo(hdc: HDC, x: i32, y: i32) -> BOOL;
    pub fn SetTextColor(hdc: HDC, c: COLORREF) -> COLORREF;
    pub fn SetBkMode(hdc: HDC, mode: i32) -> i32;
    pub fn DrawTextW(hdc: HDC, s: *const u16, n: i32, r: *mut RECT, fmt: UINT) -> i32;
    pub fn CreateFontW(
        cHeight: i32,
        cWidth: i32,
        cEscapement: i32,
        cOrientation: i32,
        cWeight: i32,
        bItalic: DWORD,
        bUnderline: DWORD,
        bStrikeOut: DWORD,
        iCharSet: DWORD,
        iOutPrecision: DWORD,
        iClipPrecision: DWORD,
        iQuality: DWORD,
        iPitchAndFamily: DWORD,
        pszFaceName: *const u16,
    ) -> HFONT;
}

// ------------------------------ kernel32 ------------------------------
#[link(name = "kernel32")]
extern "system" {
    pub fn GetModuleHandleW(lpModuleName: *const u16) -> HINSTANCE;
}

// ------------------------------ winmm ------------------------------
#[link(name = "winmm")]
extern "system" {
    pub fn waveOutOpen(
        phwo: *mut HWAVEOUT,
        uDeviceID: UINT,
        pwfx: *const WAVEFORMATEX,
        dwCallback: DWORD_PTR,
        dwInstance: DWORD_PTR,
        fdwOpen: DWORD,
    ) -> u32;
    pub fn waveOutPrepareHeader(hwo: HWAVEOUT, pwh: *mut WAVEHDR, cbwh: UINT) -> u32;
    pub fn waveOutUnprepareHeader(hwo: HWAVEOUT, pwh: *mut WAVEHDR, cbwh: UINT) -> u32;
    pub fn waveOutWrite(hwo: HWAVEOUT, pwh: *mut WAVEHDR, cbwh: UINT) -> u32;
    pub fn waveOutReset(hwo: HWAVEOUT) -> u32;
    pub fn waveOutClose(hwo: HWAVEOUT) -> u32;

    pub fn midiInGetNumDevs() -> UINT;
    pub fn midiInOpen(
        phmi: *mut HMIDIIN,
        uDeviceID: UINT,
        dwCallback: DWORD_PTR,
        dwInstance: DWORD_PTR,
        fdwOpen: DWORD,
    ) -> u32;
    pub fn midiInStart(hmi: HMIDIIN) -> u32;
    pub fn midiInStop(hmi: HMIDIIN) -> u32;
    pub fn midiInReset(hmi: HMIDIIN) -> u32;
    pub fn midiInClose(hmi: HMIDIIN) -> u32;
}

// ------------------------------ comdlg32 ------------------------------
#[link(name = "comdlg32")]
extern "system" {
    pub fn GetOpenFileNameW(lpofn: *mut OPENFILENAMEW) -> BOOL;
    pub fn GetSaveFileNameW(lpofn: *mut OPENFILENAMEW) -> BOOL;
}

// ------------------------------ 小工具 ------------------------------

/// UTF-8 → UTF-16（带结尾 0），给所有 W 后缀 API 用。
/// UI 只在绘图/弹窗时调用，分配开销可以忽略。
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Win32 的 MAKEINTRESOURCE：把整数资源 ID 伪装成字符串指针。
pub fn make_int_resource(id: usize) -> *const u16 {
    id as *const u16
}

/// 从鼠标 lParam 里取有符号的 x / y（等价 GET_X_LPARAM / GET_Y_LPARAM）
pub fn loword_signed(v: isize) -> i32 {
    (v as u16) as i16 as i32
}
pub fn hiword_signed(v: isize) -> i32 {
    ((v >> 16) as u16) as i16 as i32
}
