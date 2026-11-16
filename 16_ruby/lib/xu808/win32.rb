# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/win32.rb
# 作用：用 Ruby 标准库 Fiddle 直接绑定 user32 / gdi32 / kernel32 / winmm。
# 为什么用 Fiddle？——不装任何 gem，Ruby 自带；不需要编译 C 扩展。
# 徐某提醒：x64 下句柄/指针都是 8 字节，结构体打包必须按 8 字节对齐，
#            下面每个 pack 格式都标了字段偏移，改之前先数一遍字节。
# =============================================================================
require 'fiddle'
require 'fiddle/import'

module Xu808
  module Win32
    extend Fiddle::Importer

    # 只加载用得到的系统 DLL；缺一个都会在启动时立刻报错，便于定位。
    dlload 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'winmm.dll'

    # ---- kernel32 ----------------------------------------------------------
    extern 'void* GetModuleHandleW(void*)'          # 取本进程模块句柄
    extern 'void Sleep(unsigned long)'              # 毫秒级休眠（音频线程轮询用）
    extern 'void* CreateEventW(void*, int, int, void*)' # 音频完成事件
    extern 'unsigned int WaitForSingleObject(void*, unsigned int)'
    extern 'int CloseHandle(void*)'

    # ---- user32 ------------------------------------------------------------
    extern 'int RegisterClassExW(void*)'                                    # 注册窗口类
    extern 'void* CreateWindowExW(unsigned long, void*, void*, unsigned long, int, int, int, int, void*, void*, void*, void*)'
    extern 'long long DefWindowProcW(void*, unsigned int, unsigned long long, long long)'
    extern 'int ShowWindow(void*, int)'                                     # 显示窗口
    extern 'int UpdateWindow(void*)'                                        # 立即重绘
    extern 'int GetMessageW(void*, void*, unsigned int, unsigned int)'      # 消息循环
    extern 'int TranslateMessage(void*)'                                    # 键盘消息翻译
    extern 'long long DispatchMessageW(void*)'                              # 派发到窗口过程
    extern 'void PostQuitMessage(int)'                                      # 退出消息循环
    extern 'int InvalidateRect(void*, void*, int)'                          # 请求重绘
    extern 'unsigned long long SetTimer(void*, unsigned long long, unsigned int, void*)'
    extern 'int KillTimer(void*, unsigned long long)'
    extern 'void* LoadImageW(void*, void*, unsigned int, int, int, unsigned int)' # 加载 app.ico
    extern 'int DestroyWindow(void*)'
    extern 'void* SetCapture(void*)'                                        # 拖动滑杆时抓鼠标
    extern 'int ReleaseCapture()'
    extern 'int GetClientRect(void*, void*)'                                # 客户区尺寸
    extern 'int AdjustWindowRect(void*, unsigned long, int)'                # 客户区 -> 窗口尺寸
    extern 'int MessageBoxW(void*, void*, void*, unsigned int)'
    extern 'int PostMessageW(void*, unsigned int, unsigned long long, long long)'
    extern 'void* GetDC(void*)'
    extern 'int ReleaseDC(void*, void*)'
    extern 'int SetWindowTextW(void*, void*)'
    extern 'long long SendMessageW(void*, unsigned int, unsigned long long, long long)'
    extern 'int DrawTextW(void*, void*, int, void*, unsigned int)'          # GDI 文字
    extern 'void* BeginPaint(void*, void*)'                                 # 开始绘制
    extern 'int EndPaint(void*, void*)'                                     # 结束绘制
    extern 'int GetTickCount64()'
    extern 'int GetSystemMetrics(int)'
    extern 'int SetProcessDPIAware()'
    extern 'unsigned int timeBeginPeriod(unsigned int)'
    extern 'unsigned int timeEndPeriod(unsigned int)'

    # ---- gdi32 -------------------------------------------------------------
    extern 'void* CreateSolidBrush(unsigned long)'                          # 实心刷子
    extern 'int DeleteObject(void*)'                                       # 删除 GDI 对象
    extern 'int FillRect(void*, void*, void*)'                             # 填矩形
    extern 'void* CreatePen(int, int, unsigned long)'                       # 画笔
    extern 'void* SelectObject(void*, void*)'
    extern 'int MoveToEx(void*, int, int, void*)'
    extern 'int LineTo(void*, int, int)'
    extern 'int Rectangle(void*, int, int, int, int)'
    extern 'int RoundRect(void*, int, int, int, int, int, int)'
    extern 'int SetBkMode(void*, int)'                                     # 透明文字背景
    extern 'unsigned long SetTextColor(void*, unsigned long)'
    extern 'void* CreateFontW(int, int, int, int, int, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, void*)'
    extern 'void* CreateCompatibleDC(void*)'                               # 双缓冲内存 DC
    extern 'void* CreateCompatibleBitmap(void*, int, int)'
    extern 'int BitBlt(void*, int, int, int, int, void*, int, int, unsigned long)'
    extern 'int SetBkColor(void*, unsigned long)'
    extern 'int Ellipse(void*, int, int, int, int)'

    # ---- winmm -------------------------------------------------------------
    extern 'unsigned int waveOutGetNumDevs()'
    extern 'unsigned int waveOutOpen(void*, unsigned int, void*, void*, void*, unsigned int)'
    extern 'unsigned int waveOutClose(void*)'
    extern 'unsigned int waveOutPrepareHeader(void*, void*, unsigned int)'
    extern 'unsigned int waveOutUnprepareHeader(void*, void*, unsigned int)'
    extern 'unsigned int waveOutWrite(void*, void*, unsigned int)'
    extern 'unsigned int waveOutReset(void*)'
    extern 'unsigned int midiInGetNumDevs()'
    extern 'unsigned int midiInOpen(void*, unsigned int, void*, void*, unsigned int)'
    extern 'unsigned int midiInClose(void*)'
    extern 'unsigned int midiInStart(void*)'
    extern 'unsigned int midiInStop(void*)'

    # Windows 消息常量（只列 UI 用得到的）。
    WM_DESTROY      = 0x0002
    WM_PAINT        = 0x000F
    WM_CLOSE        = 0x0010
    WM_ERASEBKGND   = 0x0014
    WM_KEYDOWN      = 0x0100
    WM_TIMER        = 0x0113
    WM_MOUSEMOVE    = 0x0200
    WM_LBUTTONDOWN  = 0x0201
    WM_LBUTTONUP    = 0x0202
    WM_RBUTTONDOWN  = 0x0204
    WM_MOUSEWHEEL   = 0x020A
    WM_SETICON      = 0x0080
    WM_GETMINMAXINFO = 0x0024

    # 窗口样式
    WS_OVERLAPPEDWINDOW = 0x00CF0000
    WS_VISIBLE          = 0x10000000
    SW_SHOW             = 5
    SW_SHOWNORMAL       = 1

    # SetTimer / SendMessage 用
    TIMER_ID = 1

    # LoadImage 标志
    IMAGE_ICON     = 1
    LR_LOADFROMFILE = 0x00000010
    LR_DEFAULTSIZE  = 0x00000040

    # 把 Ruby 字符串转成以 \0\0 结尾的 UTF-16LE，供 *W 系列 API 使用。
    # 必须保留返回值引用到调用结束，否则 GC 可能提前回收缓冲区。
    def self.wstr(s)
      s.to_s.encode('UTF-16LE').b + "\x00\x00".b
    end

    # 打包 WNDCLASSEXW（80 字节）。偏移：0 cbSize / 8 lpfnWndProc / 16 cbClsExtra
    # / 20 cbWndExtra / 24 hInstance / 32 hIcon / 40 hCursor / 48 hbrBackground
    # / 56 lpszMenuName / 64 lpszClassName / 72 hIconSm。
    def self.wndclassex(wndproc, hinstance, icon, cursor, brush, class_name, icon_sm)
      [80, wndproc, 0, 0, hinstance, icon, cursor, brush, 0, class_name, icon_sm]
        .pack('L x4 Q L L Q Q Q Q Q Q Q')
    end

    # 打包 RECT（16 字节，四个 int32）。
    def self.rect(l, t, r, b)
      [l, t, r, b].pack('l4')
    end

    # 从 RECT 指针读回四个 int。
    def self.read_rect(ptr)
      ptr[0, 16].unpack('l4')
    end

    # MSG 结构 48 字节：hwnd(8) message(4+4pad) wParam(8) lParam(8)
    # time(4+4pad) pt(8)。GetMessageW 会往里填，我们只关心 message/wParam/lParam。
    def self.new_msg
      Fiddle::Pointer.malloc(48)
    end

    def self.read_msg(ptr)
      hwnd, message, wparam, lparam = ptr[0, 48].unpack('Q L x4 Q Q')
      [hwnd, message, wparam, lparam]
    end

    # PAINTSTRUCT 72 字节。只需要第一个字段 hdc。
    def self.new_paintstruct
      Fiddle::Pointer.malloc(72)
    end

    def self.paint_hdc(ptr)
      ptr[0, 8].unpack1('Q')
    end

    # COLORREF 是 0x00BBGGRR；这里收 RGB 三个分量再拼，读代码更直观。
    def self.rgb(r, g, b)
      (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16)
    end

    # 鼠标坐标：lParam 低 16 位 x、高 16 位 y，都是有符号。
    def self.lparam_x(lp)
      v = lp & 0xFFFF
      v >= 32_768 ? v - 65_536 : v
    end

    def self.lparam_y(lp)
      v = (lp >> 16) & 0xFFFF
      v >= 32_768 ? v - 65_536 : v
    end

    # 滚轮增量：wParam 高 16 位是有符号 short，向上滚是 +120。
    def self.wheel_delta(wp)
      v = (wp >> 16) & 0xFFFF
      v >= 32_768 ? v - 65_536 : v
    end

    # 常用 GDI 常量
    TRANSPARENT = 1
    PS_SOLID    = 0
    DT_LEFT     = 0x0000
    DT_CENTER   = 0x0001
    DT_RIGHT    = 0x0002
    DT_VCENTER  = 0x0004
    DT_SINGLELINE = 0x0020
    SRCCOPY     = 0x00CC0020

    # 当前毫秒数，用于 UI 的“自动关窗”和调试计时。
    def self.now_ms
      GetTickCount64()
    end
  end
end
