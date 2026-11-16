# -*- coding: utf-8 -*-
"""
Windows MIDI 输入（ctypes 直接调用 winmm.dll，零第三方依赖）。

徐某为什么不用 mido/python-rtmidi：它们要么需要编译 C++ 扩展，
要么在 Python 3.14 上还没有轮子；而 winmm 是 Windows 自带的，
用 ctypes 声明几个函数就能拿到 Note On / CC / 时钟，部署时什么都不用带。

线程说明：
  winmm 在它自己的线程里回调 _midi_proc；
  我们只把 3 个字节塞进 deque，真正的解析放在 GUI 定时器里做，
  绝不在回调里碰 Qt 对象，也绝不阻塞。
"""

from __future__ import annotations

import ctypes
import threading
from collections import deque
from ctypes import wintypes
from typing import Deque, List, Optional, Tuple

# winmm 常量：这些数字来自 Windows SDK 的 mmsystem.h
MIM_DATA = 0x3C3  # 收到一条 MIDI 短消息
CALLBACK_FUNCTION = 0x00030000
MMSYSERR_NOERROR = 0
MIDI_IO_STATUS = 0x00000020

winmm = ctypes.WinDLL("winmm.dll")  # 系统自带 DLL，随进程加载

# 声明函数原型；不声明的话 64 位下指针会被截断成 32 位，直接崩
# midiInOpen(lphMidiIn, uDeviceID, dwCallback, dwInstance, dwFlags)
winmm.midiInOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT,
                             ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD]
winmm.midiInOpen.restype = wintypes.UINT
winmm.midiInClose.argtypes = [wintypes.HANDLE]
winmm.midiInClose.restype = wintypes.UINT
winmm.midiInStart.argtypes = [wintypes.HANDLE]
winmm.midiInStart.restype = wintypes.UINT
winmm.midiInStop.argtypes = [wintypes.HANDLE]
winmm.midiInStop.restype = wintypes.UINT
winmm.midiInReset.argtypes = [wintypes.HANDLE]
winmm.midiInReset.restype = wintypes.UINT
winmm.midiInGetNumDevs.restype = wintypes.UINT

# 回调类型：void CALLBACK(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2)
MIDIINPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT,
                                ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)


class XuMidiIn:
    """一个尽量简单的 MIDI 输入端口封装。"""

    def __init__(self, device_index: int = 0) -> None:
        self.handle = wintypes.HANDLE(0)
        # deque 的 append 在 CPython 里是原子操作，C 回调线程写、GUI 线程读是安全的
        self.queue: Deque[Tuple[int, int, int]] = deque(maxlen=4096)
        self._proc = MIDIINPROC(self._callback)  # 必须保持引用，否则会被 GC 回收
        self.ok = False
        self.error = ""
        self.device_index = int(device_index)
        self._open()

    def _callback(self, hmidi, msg, instance, p1, p2) -> None:
        """winmm 回调：只做最轻的入队动作。"""
        if msg == MIM_DATA:
            status = p1 & 0xFF
            d1 = (p1 >> 8) & 0xFF
            d2 = (p1 >> 16) & 0xFF
            self.queue.append((status, d1, d2))
        # 其它消息（如 MIM_OPEN/MIM_CLOSE）一律忽略

    def _open(self) -> None:
        """打开设备；没有 MIDI 设备时安静地失败，UI 会显示“未连接”。"""
        n = int(winmm.midiInGetNumDevs())
        if n <= 0:
            self.error = "系统没有 MIDI 输入设备"
            return
        idx = min(max(0, self.device_index), n - 1)
        rc = winmm.midiInOpen(ctypes.byref(self.handle), idx, self._proc, 0,
                              CALLBACK_FUNCTION | MIDI_IO_STATUS)
        if rc != MMSYSERR_NOERROR:
            self.error = f"midiInOpen 失败，错误码 {rc}"
            return
        rc = winmm.midiInStart(self.handle)
        if rc != MMSYSERR_NOERROR:
            self.error = f"midiInStart 失败，错误码 {rc}"
            winmm.midiInClose(self.handle)
            return
        self.ok = True

    def poll(self) -> List[Tuple[int, int, int]]:
        """GUI 线程定时调用：一次取空队列。"""
        out: List[Tuple[int, int, int]] = []
        try:
            while True:
                out.append(self.queue.popleft())
        except IndexError:
            pass
        return out

    def close(self) -> None:
        """关闭端口；退出时必须调用，否则 winmm 可能让进程残留。"""
        if self.ok:
            try:
                winmm.midiInStop(self.handle)
                winmm.midiInReset(self.handle)
                winmm.midiInClose(self.handle)
            except Exception:
                pass
            self.ok = False
