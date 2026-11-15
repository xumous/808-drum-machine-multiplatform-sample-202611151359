#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python 3 + Tkinter 版 808 鼓机
-------------------------------------------------------------------------------
音频：808core.dll（miniaudio + core808.c）内部完成所有 DSP 与实时回调，
      Python 仅通过 ctypes 传入参数 / 读取状态，音频线程完全不经过 Python。
界面：Tkinter Canvas 自绘，33ms 刷新，播放头/电平表与音频线程解耦。
"""
import ctypes
import os
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox

# ---------------------------------------------------------------------------
# 常量（与 core808.h 保持一致）
# ---------------------------------------------------------------------------
TRACKS = 16
STEPS = 64
PATTERNS = 256
P_TUNE, P_DECAY, P_TONE, P_CHAR, P_LEVEL, P_PAN = range(6)
M_VOLUME, M_DRIVE, M_HP, M_LP, M_RESO, M_ACCENT = range(6)
F_ON, F_VEL, F_ACCENT, F_PROB, F_RATCHET, F_RATE, F_COND, F_CONDN, F_MICRO = range(9)

TRACK_NAMES = ["KICK", "SNARE", "CLAP", "RIM", "TOM LO", "TOM MID", "TOM HI", "CONGA",
               "COWBELL", "SHAKER", "TAMB", "CRASH", "RIDE", "HAT OPEN", "HAT CLOSED", "CLAVE"]
KNOB_NAMES = ["TUNE", "DECAY", "TONE", "CHAR", "LEVEL", "PAN"]
MASTER_NAMES = ["VOLUME", "DRIVE", "HP", "LP", "RESO", "ACCENT"]
COND_NAMES = ["ALWAYS", "1:2", "1:3", "1:4", "FILL", "NOT FILL", "EVERY N"]

C_BG = "#0e1014"
C_PANEL = "#1a1d23"
C_PANEL2 = "#24282f"
C_PANEL3 = "#2f343e"
C_TEXT = "#e4e7ed"
C_DIM = "#8b92a0"
C_ORANGE = "#ff7a30"
C_ORANGE2 = "#7a3a18"
C_GREEN = "#46c46a"
C_RED = "#e5484d"
C_BLUE = "#569cf5"
C_CELL = "#2a2e37"


class C808Status(ctypes.Structure):
    _fields_ = [
        ("playing", ctypes.c_int),
        ("pattern", ctypes.c_int),
        ("step", ctypes.c_int),
        ("songPos", ctypes.c_int),
        ("loopCount", ctypes.c_int),
        ("xruns", ctypes.c_int),
        ("cpuLoad", ctypes.c_float),
        ("meter", ctypes.c_float * TRACKS),
        ("masterL", ctypes.c_float),
        ("masterR", ctypes.c_float),
    ]


def resource_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def bundle_dir():
    return getattr(sys, "_MEIPASS", resource_dir())


class Core:
    """ctypes 绑定 + 便捷封装"""

    def __init__(self):
        dll = os.path.join(bundle_dir(), "808core.dll")
        if not os.path.exists(dll):
            dll = os.path.join(resource_dir(), "808core.dll")
        self.lib = ctypes.CDLL(dll)
        L = self.lib
        L.p808_open.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]
        L.p808_open.restype = ctypes.c_void_p
        L.p808_close.argtypes = []
        L.p808_close.restype = None
        L.c808_create.argtypes = [ctypes.c_double]
        L.c808_create.restype = ctypes.c_void_p
        L.c808_destroy.argtypes = [ctypes.c_void_p]
        L.c808_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float),
                                  ctypes.POINTER(ctypes.c_float), ctypes.c_int]
        L.c808_get_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(C808Status)]
        L.c808_get_status.restype = None
        for name in ("set_step_on", "set_step_vel", "set_step_accent", "set_step_prob",
                     "set_step_ratchet", "set_step_rate", "set_step_micro",
                     "set_track_len", "set_mute", "set_solo", "select_pattern",
                     "set_song_on", "set_song_len", "set_pattern_count",
                     "clear_pattern", "clear_track", "copy_pattern",
                     "transport_play", "transport_stop", "transport_stop_reset",
                     "note_on", "panic"):
            fn = getattr(L, "c808_" + name)
            fn.restype = ctypes.c_int
        L.c808_set_step_on.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_vel.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_accent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_prob.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_ratchet.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_rate.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_micro.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_set_step_cond.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int]
        L.c808_set_track_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_float]
        L.c808_set_master.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
        L.c808_set_bpm.argtypes = [ctypes.c_void_p, ctypes.c_float]
        L.c808_set_timesig.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_pattern_len.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_pattern_swing.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_track_len.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_song_entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_clear_pattern.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_clear_track.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_copy_pattern.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_select_pattern.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_set_song_on.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_set_song_len.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_set_pattern_count.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_set_track_len.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_mute.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_solo.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_transport_play.argtypes = [ctypes.c_void_p]
        L.c808_transport_stop.argtypes = [ctypes.c_void_p]
        L.c808_transport_stop_reset.argtypes = [ctypes.c_void_p]
        L.c808_note_on.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_panic.argtypes = [ctypes.c_void_p]
        L.c808_set_timesig.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_set_clock_source.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_set_track_len.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_get_clock_source.argtypes = [ctypes.c_void_p]
        L.c808_get_clock_source.restype = ctypes.c_int
        L.c808_get_step.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_get_step.restype = ctypes.c_int
        L.c808_get_track_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.c808_get_track_param.restype = ctypes.c_float
        L.c808_get_track_len.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_track_len.restype = ctypes.c_int
        L.c808_get_track_mute.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_track_mute.restype = ctypes.c_int
        L.c808_get_track_solo.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_track_solo.restype = ctypes.c_int
        L.c808_get_track_note.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_track_note.restype = ctypes.c_int
        L.c808_get_master.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_master.restype = ctypes.c_float
        L.c808_get_bpm.argtypes = [ctypes.c_void_p]
        L.c808_get_bpm.restype = ctypes.c_float
        L.c808_get_pattern_len.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_pattern_len.restype = ctypes.c_int
        L.c808_get_pattern_swing.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_get_pattern_swing.restype = ctypes.c_int
        L.c808_get_current_pattern.argtypes = [ctypes.c_void_p]
        L.c808_get_current_pattern.restype = ctypes.c_int
        L.c808_get_pattern_count.argtypes = [ctypes.c_void_p]
        L.c808_get_pattern_count.restype = ctypes.c_int
        L.c808_get_song_on.argtypes = [ctypes.c_void_p]
        L.c808_get_song_on.restype = ctypes.c_int
        L.c808_get_song_len.argtypes = [ctypes.c_void_p]
        L.c808_get_song_len.restype = ctypes.c_int
        L.c808_get_song_entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                          ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
        L.c808_get_song_entry.restype = ctypes.c_int
        L.c808_get_timesig.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.c_int)]
        L.c808_get_timesig.restype = ctypes.c_int
        L.c808_save_project.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        L.c808_save_project.restype = ctypes.c_int
        L.c808_load_project.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        L.c808_load_project.restype = ctypes.c_int
        L.c808_export_wav.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.c808_export_wav.restype = ctypes.c_int
        L.c808_export_midi.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        L.c808_export_midi.restype = ctypes.c_int
        L.c808_midi_device_count.restype = ctypes.c_int
        L.c808_midi_device_name.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        L.c808_midi_device_name.restype = ctypes.c_int
        L.c808_midi_start.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.c808_midi_start.restype = ctypes.c_int
        L.c808_midi_stop.argtypes = [ctypes.c_void_p]
        L.c808_midi_stop.restype = None
        self.engine = None
        self.audio_ok = False
        self.sample_rate = 48000
        self.sr = 48000
        self.audio_name = "NO AUDIO"
        self._dummy_l = (ctypes.c_float * 256)()
        self._dummy_r = (ctypes.c_float * 256)()

    def open(self):
        sr = ctypes.c_int(48000)
        err = ctypes.create_string_buffer(256)
        eng = self.lib.p808_open(ctypes.byref(sr), err, 256)
        if eng:
            self.engine = eng
            self.audio_ok = True
            self.sample_rate = sr.value
            self.audio_name = "miniaudio/WASAPI %d Hz · 2 ch" % sr.value
        else:
            self.audio_ok = False
            self.engine = self.lib.c808_create(48000.0)
            self.sample_rate = 48000
            self.audio_name = "AUDIO FAILED: " + err.value.decode("utf-8", "replace")
        self.sr = self.sample_rate
        return self.engine is not None

    def close(self):
        if self.audio_ok:
            self.lib.p808_close()
        elif self.engine:
            self.lib.c808_destroy(self.engine)
        self.engine = None

    def pump(self):
        if not self.audio_ok and self.engine:
            self.lib.c808_render(self.engine, self._dummy_l, self._dummy_r, 256)

    def status(self):
        st = C808Status()
        self.lib.c808_get_status(self.engine, ctypes.byref(st))
        return st

    def step(self, pat, t, s, field):
        return self.lib.c808_get_step(self.engine, pat, t, s, field)

    def track_param(self, t, p):
        return self.lib.c808_get_track_param(self.engine, t, p)

    def master(self, p):
        return self.lib.c808_get_master(self.engine, p)

    def bpm(self):
        return self.lib.c808_get_bpm(self.engine)

    def pattern(self):
        return self.lib.c808_get_current_pattern(self.engine)

    def pattern_len(self, pat):
        return self.lib.c808_get_pattern_len(self.engine, pat)

    def swing(self, pat):
        return self.lib.c808_get_pattern_swing(self.engine, pat)

    def track_len(self, t):
        return self.lib.c808_get_track_len(self.engine, t)

    def mute(self, t):
        return self.lib.c808_get_track_mute(self.engine, t)

    def solo(self, t):
        return self.lib.c808_get_track_solo(self.engine, t)

    def song_on(self):
        return self.lib.c808_get_song_on(self.engine)

    def song_len(self):
        return self.lib.c808_get_song_len(self.engine)

    def song_entry(self, i):
        pat = ctypes.c_int()
        reps = ctypes.c_int()
        if self.lib.c808_get_song_entry(self.engine, i, ctypes.byref(pat), ctypes.byref(reps)) != 0:
            return (0, 1)
        return (pat.value, reps.value)

    def timesig(self):
        n = ctypes.c_int(4)
        d = ctypes.c_int(4)
        self.lib.c808_get_timesig(self.engine, ctypes.byref(n), ctypes.byref(d))
        return (n.value, d.value)

    def set_step(self, pat, t, s, field, value):
        if field == F_ON:
            self.lib.c808_set_step_on(self.engine, pat, t, s, int(value))
        elif field == F_VEL:
            self.lib.c808_set_step_vel(self.engine, pat, t, s, int(value))
        elif field == F_ACCENT:
            self.lib.c808_set_step_accent(self.engine, pat, t, s, int(value))
        elif field == F_PROB:
            self.lib.c808_set_step_prob(self.engine, pat, t, s, int(value))
        elif field == F_RATCHET:
            self.lib.c808_set_step_ratchet(self.engine, pat, t, s, int(value))
        elif field == F_RATE:
            self.lib.c808_set_step_rate(self.engine, pat, t, s, int(value))
        elif field == F_MICRO:
            self.lib.c808_set_step_micro(self.engine, pat, t, s, int(value))

    def set_cond(self, pat, t, s, cond, cond_n):
        self.lib.c808_set_step_cond(self.engine, pat, t, s, int(cond), int(cond_n))

    def set_track(self, t, p, value):
        self.lib.c808_set_track_param(self.engine, t, p, float(value))

    def set_master(self, p, value):
        self.lib.c808_set_master(self.engine, p, float(value))

    def set_bpm(self, bpm):
        self.lib.c808_set_bpm(self.engine, float(bpm))

    def set_pattern_len(self, pat, n):
        self.lib.c808_set_pattern_len(self.engine, pat, int(n))

    def set_swing(self, pat, n):
        self.lib.c808_set_pattern_swing(self.engine, pat, int(n))

    def set_timesig(self, num, den):
        self.lib.c808_set_timesig(self.engine, int(num), int(den))

    def set_clock(self, midi):
        self.lib.c808_set_clock_source(self.engine, 1 if midi else 0)

    def clock_source(self):
        return self.lib.c808_get_clock_source(self.engine)

    def set_track_len(self, t, n):
        self.lib.c808_set_track_len(self.engine, int(t), int(n))

    def set_mute(self, t, v):
        self.lib.c808_set_mute(self.engine, t, int(v))

    def set_solo(self, t, v):
        self.lib.c808_set_solo(self.engine, t, int(v))

    def select(self, pat):
        self.lib.c808_select_pattern(self.engine, int(pat))

    def play(self):
        self.lib.c808_transport_play(self.engine)

    def stop(self):
        self.lib.c808_transport_stop(self.engine)

    def stop_reset(self):
        self.lib.c808_transport_stop_reset(self.engine)

    def note_on(self, t, vel=112):
        self.lib.c808_note_on(self.engine, int(t), int(vel), 0)

    def song_set(self, on):
        self.lib.c808_set_song_on(self.engine, 1 if on else 0)

    def export_wav(self, path, mode, loops, tail, stems):
        return self.lib.c808_export_wav(self.engine, path.encode("utf-8"), int(mode),
                                        int(loops), int(tail), int(stems))

    def export_midi(self, path, mode, loops):
        return self.lib.c808_export_midi(self.engine, path.encode("utf-8"), int(mode), int(loops))

    def save(self, path):
        return self.lib.c808_save_project(self.engine, path.encode("utf-8"))

    def load(self, path):
        return self.lib.c808_load_project(self.engine, path.encode("utf-8"))


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


class App:
    def __init__(self, root, core):
        self.root = root
        self.core = core
        self.sel_track = 0
        self.sel_step = 0
        self.page = 0
        self.scale = 1.0
        self.drag = None
        self.msg = "READY"
        self.midi_name = "MIDI: OFF"
        self.midi_index = -1
        self.export_busy = False
        self.cells = {}
        self.markers = {}
        self.grid_len = 0
        self.items = {}

        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            pass

        self.root.title("808 · PYTHON SYNTHETIC DRUM MACHINE")
        self.root.configure(bg=C_BG)
        self.root.geometry("1180x700")
        self.root.minsize(820, 500)

        self.canvas = tk.Canvas(root, bg=C_BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.focus_set()

        self.canvas.bind("<Configure>", self.on_resize)
        self.canvas.bind("<Button-1>", self.on_click)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.root.bind("<KeyPress>", self.on_key)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.midi_init()
        self.rebuild()
        self.tick()

    # ------------------------------------------------------------------
    # 布局与绘制
    # ------------------------------------------------------------------
    def S(self, v):
        return v * self.scale

    def rect(self, x, y, w, h):
        return (int(x * self.scale), int(y * self.scale),
                int((x + w) * self.scale), int((y + h) * self.scale))

    def on_resize(self, _event):
        w = self.canvas.winfo_width()
        h = self.canvas.winfo_height()
        if w < 10 or h < 10:
            return
        ns = min(w / 1280.0, h / 720.0)
        ns = clamp(ns, 0.35, 3.0)
        if abs(ns - self.scale) > 0.01:
            self.scale = ns
            self.rebuild()

    def font(self, px, bold=False):
        fam = "Segoe UI"
        return (fam, -max(6, int(px * self.scale)), "bold" if bold else "normal")

    def build_rects(self):
        s = self.scale
        self.rc = {}
        self.rc["play"] = self.rect(12, 8, 72, 30)
        self.rc["stop"] = self.rect(90, 8, 72, 30)
        self.rc["tap"] = self.rect(168, 8, 60, 30)
        self.rc["bpm_minus"] = self.rect(234, 8, 30, 30)
        self.rc["bpm"] = self.rect(268, 8, 86, 30)
        self.rc["bpm_plus"] = self.rect(358, 8, 30, 30)
        self.rc["swing"] = self.rect(400, 8, 190, 30)
        self.rc["pat_prev"] = self.rect(600, 8, 30, 30)
        self.rc["pat_disp"] = self.rect(634, 8, 92, 30)
        self.rc["pat_next"] = self.rect(730, 8, 30, 30)
        self.rc["len_minus"] = self.rect(772, 8, 30, 30)
        self.rc["len_disp"] = self.rect(806, 8, 70, 30)
        self.rc["len_plus"] = self.rect(880, 8, 30, 30)
        self.rc["song"] = self.rect(922, 8, 72, 30)
        self.rc["song_edit"] = self.rect(1000, 8, 84, 30)
        self.rc["save"] = self.rect(12, 44, 70, 28)
        self.rc["load"] = self.rect(86, 44, 70, 28)
        self.rc["wav"] = self.rect(160, 44, 70, 28)
        self.rc["midi"] = self.rect(234, 44, 70, 28)
        self.rc["stems"] = self.rect(308, 44, 76, 28)
        self.rc["clear"] = self.rect(388, 44, 64, 28)
        self.rc["copy"] = self.rect(456, 44, 64, 28)
        self.rc["midi_in"] = self.rect(524, 44, 240, 28)
        self.rc["sync"] = self.rect(772, 44, 76, 28)
        self.rc["timesig"] = self.rect(854, 44, 70, 28)
        self.rc["trklen_m"] = self.rect(860, 486, 32, 26)
        self.rc["trklen_d"] = self.rect(896, 486, 128, 26)
        self.rc["trklen_p"] = self.rect(1028, 486, 32, 26)
        self.rc["grid"] = self.rect(12, 84, 830, 432)
        self.rc["panel"] = self.rect(852, 84, 416, 432)
        self.rc["master"] = self.rect(12, 526, 830, 106)
        self.rc["pads"] = self.rect(852, 526, 416, 106)
        self.rc["song_strip"] = self.rect(12, 640, 1256, 40)
        self.rc["status"] = self.rect(0, 686, 1280, 34)
        gx1, gy1, gx2, gy2 = self.rc["grid"]
        self.rc["grid_steps"] = (gx1 + int(126 * s), gy1 + int(22 * s),
                                 gx2 - int(8 * s), gy2 - int(6 * s))
        for i in range(6):
            col, row = i % 3, i // 3
            self.rc["vk%d" % i] = self.rect(862 + col * 134, 118 + row * 80, 128, 78)
        for i in range(6):
            self.rc["mk%d" % i] = self.rect(30 + i * 134, 542, 128, 86)
        for i in range(5):
            self.rc["ss%d" % i] = self.rect(860, 316 + i * 25, 400, 24)
        self.rc["cond"] = self.rect(860, 452, 210, 28)
        self.rc["accent"] = self.rect(1080, 452, 180, 28)
        self.rc["trigger"] = self.rect(1090, 94, 168, 24)
        for i in range(16):
            col, row = i % 4, i // 4
            self.rc["pad%d" % i] = self.rect(858 + col * 102, 530 + row * 25, 98, 22)
        for i in range(16):
            self.rc["pq%d" % i] = self.rect(34 + i * 76, 644, 70, 30)
        self.rc["page_prev"] = self.rect(14, 644, 16, 30)
        self.rc["page_next"] = self.rect(1252, 644, 16, 30)

    def clear_canvas(self):
        self.canvas.delete("all")
        self.cells = {}
        self.markers = {}
        self.items = {}

    def rebuild(self):
        self.clear_canvas()
        self.play_ids = []
        self.sel_id = None
        self.build_rects()
        self.draw_static()
        self.dirty = True
        self.update_ui()

    def draw_rect(self, key, r, fill, outline=None, width=1, tags=()):
        if outline is None:
            outline = fill
        x1, y1, x2, y2 = r
        iid = self.canvas.create_rectangle(x1, y1, x2, y2, fill=fill,
                                           outline=outline, width=width, tags=tags)
        self.items[key] = iid
        return iid

    def draw_text(self, key, r, text, color, font, anchor="center"):
        x1, y1, x2, y2 = r
        if anchor == "center":
            x, y = (x1 + x2) / 2, (y1 + y2) / 2
        elif anchor == "w":
            x, y = x1 + 4, (y1 + y2) / 2
        else:
            x, y = x2 - 4, (y1 + y2) / 2
        iid = self.canvas.create_text(x, y, text=text, fill=color, font=font,
                                      anchor=anchor[0] if anchor in ("w", "e") else "center")
        self.items[key] = iid
        return iid

    def button(self, key, r, text, active=False, font_px=13):
        x1, y1, x2, y2 = r
        self.draw_rect(key + "_bg", r, C_ORANGE if active else C_PANEL2,
                       C_ORANGE if active else C_PANEL3)
        self.draw_text(key + "_tx", r, text, "#14100c" if active else C_TEXT,
                       self.font(font_px, True))
        (x1, y1, x2, y2)

    def draw_knob(self, key, r, label, value):
        x1, y1, x2, y2 = r
        cx = (x1 + x2) / 2
        rad = min((y2 - y1 - self.S(38)) / 2, self.S(20))
        rad = max(rad, self.S(7))
        cy = y1 + self.S(4) + rad
        self.canvas.create_oval(cx - rad, cy - rad, cx + rad, cy + rad,
                                fill=C_PANEL3, outline=C_ORANGE2, width=max(2, int(self.S(4))),
                                tags=(key + "_oval",))
        self.canvas.create_arc(cx - rad - self.S(5), cy - rad - self.S(5),
                               cx + rad + self.S(5), cy + rad + self.S(5),
                               start=225, extent=-270, style="arc", outline=C_ORANGE,
                               width=max(2, int(self.S(2.5))), tags=(key + "_arc",))
        import math
        ang = math.radians(135 + 270 * value)
        px = cx + math.cos(ang) * (rad - self.S(4))
        py = cy - math.sin(ang) * (rad - self.S(4))
        self.items[key + "_ptr"] = self.canvas.create_line(
            cx, cy, px, py, fill=C_TEXT, width=max(2, int(self.S(2.5))))
        self.canvas.create_text(cx, cy + rad + self.S(8), text=label, fill=C_DIM,
                                font=self.font(9), tags=(key + "_lbl",))
        self.items[key + "_val"] = self.canvas.create_text(
            cx, cy + rad + self.S(22), text=str(int(value * 100 + 0.5)),
            fill=C_TEXT, font=self.font(11))

    def draw_slider(self, key, r, label, value, text, bipolar=False):
        x1, y1, x2, y2 = r
        self.canvas.create_text(x1 + self.S(4), (y1 + y2) / 2, text=label, anchor="w",
                                fill=C_DIM, font=self.font(11), tags=(key + "_lbl",))
        self.items[key + "_val"] = self.canvas.create_text(
            x2 - self.S(4), (y1 + y2) / 2, text=text, anchor="e",
            fill=C_TEXT, font=self.font(11))
        bx1 = x1 + self.S(78)
        bx2 = x2 - self.S(60)
        by1 = y1 + self.S(7)
        by2 = y2 - self.S(7)
        self.canvas.create_rectangle(bx1, by1, bx2, by2, fill=C_PANEL3, outline=C_PANEL3,
                                     tags=(key + "_bar",))
        if bipolar:
            mid = (bx1 + bx2) / 2
            self.canvas.create_line(mid, by1 - self.S(2), mid, by2 + self.S(2),
                                    fill=C_DIM, tags=(key + "_mid",))
            fill = self.canvas.create_rectangle(mid, by1, mid, by2, fill=C_ORANGE,
                                                outline=C_ORANGE, tags=(key + "_fill",))
        else:
            fill = self.canvas.create_rectangle(bx1, by1, bx1, by2, fill=C_ORANGE,
                                                outline=C_ORANGE, tags=(key + "_fill",))
        self.items[key + "_fill"] = fill

    def update_slider(self, key, value, bipolar=False):
        pass

    def draw_static(self):
        s = self.scale
        # 顶部
        self.button("play", self.rc["play"], "PLAY", False)
        self.button("stop", self.rc["stop"], "STOP", False)
        self.button("tap", self.rc["tap"], "TAP", False)
        self.button("bpm_minus", self.rc["bpm_minus"], "-", False)
        self.button("bpm_plus", self.rc["bpm_plus"], "+", False)
        self.draw_rect("bpm_bg", self.rc["bpm"], C_PANEL2, C_PANEL3)
        self.draw_text("bpm_tx", self.rc["bpm"], "120.0 BPM", C_ORANGE, self.font(13, True))
        self.draw_text("swing_lbl", self.rect(400, 8, 52, 30), "SWING", C_DIM,
                       self.font(11), anchor="w")
        self.draw_rect("swing_bg", self.rect(454, 17, 136, 12), C_PANEL3, C_PANEL3)
        self.draw_rect("swing_fill", self.rect(454, 17, 0, 12), C_ORANGE, C_ORANGE)
        self.draw_text("swing_val", self.rect(564, 8, 26, 30), "0%", C_TEXT, self.font(11))

        self.button("pat_prev", self.rc["pat_prev"], "<", False)
        self.button("pat_next", self.rc["pat_next"], ">", False)
        self.draw_rect("pat_bg", self.rc["pat_disp"], C_PANEL2, C_PANEL3)
        self.draw_text("pat_tx", self.rc["pat_disp"], "PAT 000", C_TEXT, self.font(13, True))
        self.button("len_minus", self.rc["len_minus"], "-", False)
        self.button("len_plus", self.rc["len_plus"], "+", False)
        self.draw_rect("len_bg", self.rc["len_disp"], C_PANEL2, C_PANEL3)
        self.draw_text("len_tx", self.rc["len_disp"], "LEN 16", C_TEXT, self.font(13, True))
        self.button("song", self.rc["song"], "SONG OFF", False)
        self.button("song_edit", self.rc["song_edit"], "EDIT SONG", False)

        self.button("save", self.rc["save"], "SAVE", False)
        self.button("load", self.rc["load"], "LOAD", False)
        self.button("wav", self.rc["wav"], "WAV", False)
        self.button("midi", self.rc["midi"], "MIDI", False)
        self.button("stems", self.rc["stems"], "STEMS", False)
        self.button("clear", self.rc["clear"], "CLEAR", False)
        self.button("copy", self.rc["copy"], "COPY>", False)
        self.draw_rect("midi_in_bg", self.rc["midi_in"], C_PANEL2, C_PANEL3)
        self.draw_text("midi_in_tx", self.rc["midi_in"], self.midi_name, C_DIM,
                       self.font(11), anchor="w")
        self.button("sync", self.rc["sync"], "SYNC INT", False, 11)
        self.button("timesig", self.rc["timesig"], "4/4", False, 11)

        # 网格
        self.draw_rect("grid_bg", self.rc["grid"], C_PANEL, C_PANEL)
        gx1, gy1, gx2, gy2 = self.rc["grid"]
        sx1, sy1, sx2, sy2 = self.rc["grid_steps"]
        self.draw_text("grid_hdr", (gx1 + int(6 * s), gy1 + int(2 * s),
                                    gx1 + int(120 * s), gy1 + int(20 * s)),
                       "TRACK", C_DIM, self.font(9), anchor="w")
        pat = self.core.pattern()
        ln = clamp(self.core.pattern_len(pat), 1, STEPS)
        self.grid_len = ln
        row_h = (sy2 - sy1) / 16.0
        col_w = (sx2 - sx1) / ln
        for step in range(0, ln, 4):
            x = sx1 + step * col_w + col_w / 2
            self.canvas.create_text(x, gy1 + int(10 * s), text=str(step + 1),
                                    fill=C_DIM, font=self.font(9))
        for t in range(16):
            y1 = sy1 + t * row_h
            y2 = y1 + row_h
            self.canvas.create_rectangle(gx1, y1, sx1, y2,
                                         fill="#1b1e24" if (t & 1) else "#171a20",
                                         outline="", tags=("row%d" % t,))
            self.canvas.create_text(gx1 + int(4 * s), (y1 + y2) / 2,
                                    text=TRACK_NAMES[t], anchor="w",
                                    fill=C_TEXT, font=self.font(9), tags=("tname%d" % t,))
            # 记录 id 以便每帧更新选中颜色
            for iid in self.canvas.find_withtag("tname%d" % t):
                self.items["tname%d" % t] = iid
            self.rc["mute%d" % t] = (gx1 + int(72 * s), int(y1 + 2 * s),
                                     gx1 + int(94 * s), int(y2 - 2 * s))
            self.rc["solo%d" % t] = (gx1 + int(96 * s), int(y1 + 2 * s),
                                     gx1 + int(118 * s), int(y2 - 2 * s))
            self.rc["meter%d" % t] = (gx1 + int(120 * s), int(y1 + 2 * s),
                                      gx1 + int(124 * s), int(y2 - 2 * s))
            self.draw_rect("mute_bg%d" % t, self.rc["mute%d" % t], C_PANEL3, C_PANEL3)
            self.draw_text("mute_tx%d" % t, self.rc["mute%d" % t], "M", C_DIM, self.font(9))
            self.draw_rect("solo_bg%d" % t, self.rc["solo%d" % t], C_PANEL3, C_PANEL3)
            self.draw_text("solo_tx%d" % t, self.rc["solo%d" % t], "S", C_DIM, self.font(9))
            self.draw_rect("meter%d" % t, self.rc["meter%d" % t], C_PANEL3, C_PANEL3)
            for st in range(ln):
                x1 = sx1 + st * col_w + int(1 * s)
                x2 = sx1 + (st + 1) * col_w - int(1 * s)
                cy1 = y1 + int(2 * s)
                cy2 = y2 - int(2 * s)
                cid = self.canvas.create_rectangle(x1, cy1, x2, cy2, fill=C_CELL,
                                                   outline=C_CELL, tags=("cell%d_%d" % (t, st),))
                self.cells[(t, st)] = cid
                self.markers[(t, st)] = {
                    "prob": self.canvas.create_rectangle(x1 + 2, cy1 + 2, x1 + 5, cy1 + 5,
                                                         fill=C_BLUE, outline="", state="hidden"),
                    "rat": [self.canvas.create_rectangle(x1 + 3 + k * 3, cy2 - 5,
                                                         x1 + 4 + k * 3, cy2 - 2,
                                                         fill=C_TEXT, outline="", state="hidden")
                            for k in range(4)],
                    "micro": self.canvas.create_rectangle(x2 - 4, cy1, x2 - 1, cy1 + 4,
                                                          fill=C_GREEN, outline="", state="hidden"),
                    "sel": self.canvas.create_rectangle(x1, cy1, x2, cy2, outline=C_TEXT,
                                                        width=max(1, int(s)), state="hidden"),
                    "play": self.canvas.create_rectangle(x1, cy1, x2, cy2, outline=C_BLUE,
                                                         width=max(1, int(1.5 * s)), state="hidden"),
                }

        # 声部面板
        self.draw_rect("panel_bg", self.rc["panel"], C_PANEL, C_PANEL)
        self.draw_text("panel_title", (self.rc["panel"][0] + int(12 * s), self.rc["panel"][1] + int(7 * s),
                                       self.rc["panel"][2] - int(190 * s), self.rc["panel"][1] + int(27 * s)),
                       "VOICE EDIT · KICK", C_ORANGE, self.font(13, True), anchor="w")
        self.button("trigger", self.rc["trigger"], "TRIGGER", False, 11)
        for i in range(6):
            self.draw_knob("vk%d" % i, self.rc["vk%d" % i], KNOB_NAMES[i], 0.5)
        self.draw_text("step_title", (self.rc["panel"][0] + int(10 * s), self.rc["panel"][1] + int(200 * s),
                                      self.rc["panel"][2] - int(10 * s), self.rc["panel"][1] + int(220 * s)),
                       "STEP 01 · KICK", C_TEXT, self.font(13, True), anchor="w")
        for i, label in enumerate(["VELOCITY", "PROB", "RATCHET", "RATE", "MICRO"]):
            self.rc["ss%d" % i] = self.rect(860, 316 + i * 25, 400, 24)
            self.draw_slider("ss%d" % i, self.rc["ss%d" % i], label, 0.5, "0", i == 4)
        self.button("cond", self.rc["cond"], "COND: ALWAYS", False, 11)
        self.button("accent", self.rc["accent"], "ACCENT", False, 11)
        self.button("trklen_m", self.rc["trklen_m"], "-", False, 11)
        self.button("trklen_d", self.rc["trklen_d"], "TRACK LEN 16", False, 11)
        self.button("trklen_p", self.rc["trklen_p"], "+", False, 11)

        # 主控
        self.draw_rect("master_bg", self.rc["master"], C_PANEL, C_PANEL)
        self.draw_text("master_lbl", (self.rc["master"][0] + int(12 * s), self.rc["master"][1] + int(2 * s),
                                      self.rc["master"][2], self.rc["master"][1] + int(16 * s)),
                       "MASTER · FILTER / SATURATION / ACCENT", C_DIM, self.font(9), anchor="w")
        for i in range(6):
            self.draw_knob("mk%d" % i, self.rc["mk%d" % i], MASTER_NAMES[i], 0.5)

        # 打击垫
        self.draw_rect("pads_bg", self.rc["pads"], C_PANEL, C_PANEL)
        for i in range(16):
            self.draw_rect("pad_bg%d" % i, self.rc["pad%d" % i], C_PANEL2, C_PANEL3)
            self.draw_text("pad_tx%d" % i, self.rc["pad%d" % i], TRACK_NAMES[i],
                           C_DIM, self.font(9))

        # pattern 快捷条
        self.draw_rect("song_strip_bg", self.rc["song_strip"], C_PANEL, C_PANEL)
        self.button("page_prev", self.rc["page_prev"], "<", False, 9)
        self.button("page_next", self.rc["page_next"], ">", False, 9)
        for i in range(16):
            self.button("pq%d" % i, self.rc["pq%d" % i], str(i), False, 11)

        # 状态
        self.draw_rect("status_bg", self.rc["status"], C_PANEL, C_PANEL)
        self.draw_text("status_tx", self.rc["status"], "", C_DIM, self.font(11), anchor="w")

    # ------------------------------------------------------------------
    # 每帧更新
    # ------------------------------------------------------------------
    def tick(self):
        try:
            self.core.pump()
            st = self.core.status()
            self.update_ui(st)
        except Exception as exc:  # 保证 UI 不因单帧异常退出
            self.msg = "ERR: %s" % exc
            print("tick error:", exc, file=sys.stderr)
        self.root.after(33, self.tick)

    def update_ui(self, st=None):
        if st is None:
            st = self.core.status()
        c = self.core
        pat = c.pattern()
        ln = clamp(c.pattern_len(pat), 1, STEPS)
        if ln != self.grid_len:
            self.rebuild()
            return
        self.frame_no = getattr(self, "frame_no", 0) + 1
        full = self.dirty or (self.frame_no % 30 == 0)
        self.dirty = False
        s = self.scale
        self.canvas.itemconfigure(self.items["play_bg"], fill=C_ORANGE if st.playing else C_PANEL2,
                                  outline=C_ORANGE if st.playing else C_PANEL3)
        self.canvas.itemconfigure(self.items["play_tx"], text="PLAYING" if st.playing else "PLAY",
                                  fill="#14100c" if st.playing else C_TEXT)
        self.canvas.itemconfigure(self.items["bpm_tx"], text="%.1f BPM" % c.bpm())
        sw = c.swing(pat)
        self.canvas.itemconfigure(self.items["pat_tx"], text="PAT %03d" % pat)
        self.canvas.itemconfigure(self.items["len_tx"], text="LEN %d" % ln)
        self.canvas.itemconfigure(self.items["song_bg"], fill=C_ORANGE if c.song_on() else C_PANEL2,
                                  outline=C_ORANGE if c.song_on() else C_PANEL3)
        self.canvas.itemconfigure(self.items["song_tx"], text="SONG ON" if c.song_on() else "SONG OFF",
                                  fill="#14100c" if c.song_on() else C_TEXT)
        self.canvas.itemconfigure(self.items["midi_in_tx"], text=self.midi_name)
        clock_midi = c.clock_source() != 0
        self.canvas.itemconfigure(self.items["sync_bg"],
                                  fill=C_ORANGE if clock_midi else C_PANEL2,
                                  outline=C_ORANGE if clock_midi else C_PANEL3)
        self.canvas.itemconfigure(self.items["sync_tx"],
                                  text="SYNC MIDI" if clock_midi else "SYNC INT",
                                  fill="#14100c" if clock_midi else C_TEXT)
        self.canvas.itemconfigure(self.items["timesig_tx"], text="%d/%d" % c.timesig())
        self.canvas.itemconfigure(self.items["trklen_d_tx"],
                                  text="TRACK LEN %d" % c.track_len(self.sel_track))
        # swing
        x1, y1, x2, y2 = self.rc["swing"]
        bx1 = x1 + int(54 * s)
        bw = int(136 * s)
        self.canvas.coords(self.items["swing_fill"], bx1, y1 + int(9 * s),
                           bx1 + int(bw * sw / 100.0), y2 - int(9 * s))
        self.canvas.itemconfigure(self.items["swing_val"], text="%d%%" % sw)

        # 网格
        sx1, sy1, sx2, sy2 = self.rc["grid_steps"]
        row_h = (sy2 - sy1) / 16.0
        col_w = (sx2 - sx1) / ln
        for t in range(16):
            # 播放头按轨道长度循环
            tlen = clamp(c.track_len(t), 1, ln)
            play_step = (st.step % tlen) if st.playing else -1
            mute = c.mute(t)
            solo = c.solo(t)
            self.canvas.itemconfigure(self.items["mute_bg%d" % t],
                                      fill=C_RED if mute else C_PANEL3)
            self.canvas.itemconfigure(self.items["mute_tx%d" % t],
                                      fill=C_TEXT if mute else C_DIM)
            self.canvas.itemconfigure(self.items["solo_bg%d" % t],
                                      fill=C_GREEN if solo else C_PANEL3)
            self.canvas.itemconfigure(self.items["solo_tx%d" % t],
                                      fill=C_TEXT if solo else C_DIM)
            self.canvas.itemconfigure(self.items["tname%d" % t],
                                      fill=C_ORANGE if t == self.sel_track else C_TEXT)
            # 电平
            mx1, my1, mx2, my2 = self.rc["meter%d" % t]
            h = (my2 - my1) * clamp(st.meter[t], 0.0, 1.0)
            self.canvas.coords(self.items["meter%d" % t], mx1, my2 - h, mx2, my2)
            self.canvas.itemconfigure(self.items["meter%d" % t],
                                      fill=C_RED if st.meter[t] > 0.9 else
                                      (C_ORANGE if st.meter[t] > 0.6 else C_GREEN))
            for step in range(ln):
                if not full:
                    break
                on = c.step(pat, t, step, F_ON)
                vel = c.step(pat, t, step, F_VEL)
                acc = c.step(pat, t, step, F_ACCENT)
                prob = c.step(pat, t, step, F_PROB)
                rat = c.step(pat, t, step, F_RATCHET)
                micro = c.step(pat, t, step, F_MICRO)
                if on:
                    v = vel / 127.0
                    col = self.blend(C_ORANGE2, C_ORANGE, v)
                    if acc:
                        col = "#ffc478"
                else:
                    col = C_CELL
                self.canvas.itemconfigure(self.cells[(t, step)], fill=col, outline=col)
                m = self.markers[(t, step)]
                self.canvas.itemconfigure(m["prob"], state="normal" if (on and prob < 100) else "hidden")
                for k in range(4):
                    self.canvas.itemconfigure(m["rat"][k],
                                              state="normal" if (on and rat > 1 and k < min(4, rat)) else "hidden")
                self.canvas.itemconfigure(m["micro"],
                                          state="normal" if (on and micro != 0) else "hidden",
                                          fill=C_GREEN if micro > 0 else C_RED)
                self.canvas.itemconfigure(m["sel"],
                                          state="normal" if (
                                                  t == self.sel_track and step == self.sel_step) else "hidden")
                self.canvas.itemconfigure(m["play"],
                                          state="normal" if (play_step == step) else "hidden")

        # 播放头与选中框：每帧单独更新，避免全网格重绘
        for t, step in getattr(self, "play_ids", []):
            self.canvas.itemconfigure(self.markers[(t, step)]["play"], state="hidden")
        self.play_ids = []
        if st.playing:
            for t in range(16):
                tlen = clamp(c.track_len(t), 1, ln)
                step = st.step % tlen
                self.canvas.itemconfigure(self.markers[(t, step)]["play"], state="normal")
                self.play_ids.append((t, step))
        if hasattr(self, "sel_id") and self.sel_id:
            self.canvas.itemconfigure(self.sel_id, state="hidden")
        self.sel_id = self.markers[(self.sel_track, self.sel_step)]["sel"]
        self.canvas.itemconfigure(self.sel_id, state="normal")

        # 声部旋钮
        self.canvas.itemconfigure(self.items["panel_title"],
                                  text="VOICE EDIT · %s" % TRACK_NAMES[self.sel_track])
        self.canvas.itemconfigure(self.items["step_title"],
                                  text="STEP %02d · %s" % (self.sel_step + 1, TRACK_NAMES[self.sel_track]))
        import math
        for i in range(6):
            val = c.track_param(self.sel_track, i)
            r = self.rc["vk%d" % i]
            x1, y1, x2, y2 = r
            rad = max(min((y2 - y1 - self.S(38)) / 2, self.S(20)), self.S(7))
            cx = (x1 + x2) / 2
            cy = y1 + self.S(4) + rad
            ang = math.radians(135 + 270 * val)
            self.canvas.coords(self.items["vk%d_ptr" % i], cx, cy,
                               cx + math.cos(ang) * (rad - self.S(4)),
                               cy - math.sin(ang) * (rad - self.S(4)))
            self.canvas.itemconfigure(self.items["vk%d_val" % i], text=str(int(val * 100 + 0.5)))
        for i in range(6):
            val = c.master(i)
            r = self.rc["mk%d" % i]
            x1, y1, x2, y2 = r
            rad = max(min((y2 - y1 - self.S(38)) / 2, self.S(20)), self.S(7))
            cx = (x1 + x2) / 2
            cy = y1 + self.S(4) + rad
            ang = math.radians(135 + 270 * val)
            self.canvas.coords(self.items["mk%d_ptr" % i], cx, cy,
                               cx + math.cos(ang) * (rad - self.S(4)),
                               cy - math.sin(ang) * (rad - self.S(4)))
            self.canvas.itemconfigure(self.items["mk%d_val" % i], text=str(int(val * 100 + 0.5)))

        # step 属性
        vel = c.step(pat, self.sel_track, self.sel_step, F_VEL)
        prob = c.step(pat, self.sel_track, self.sel_step, F_PROB)
        rat = c.step(pat, self.sel_track, self.sel_step, F_RATCHET)
        rate = c.step(pat, self.sel_track, self.sel_step, F_RATE)
        micro = c.step(pat, self.sel_track, self.sel_step, F_MICRO)
        cond = c.step(pat, self.sel_track, self.sel_step, F_COND)
        values = [(vel - 1) / 126.0, prob / 100.0, (rat - 1) / 7.0,
                  (rate - 10) / 90.0, micro / 4800.0 + 0.5]
        texts = [str(vel), "%d%%" % prob, "x%d" % rat, "%d%%" % rate, "%+d smp" % micro]
        for i in range(5):
            self.update_slider_value(i, values[i], texts[i], i == 4)
        self.canvas.itemconfigure(self.items["cond_bg"],
                                  fill=C_ORANGE if cond else C_PANEL2,
                                  outline=C_ORANGE if cond else C_PANEL3)
        self.canvas.itemconfigure(self.items["cond_tx"],
                                  text="COND: %s" % COND_NAMES[cond if cond <= 6 else 0],
                                  fill="#14100c" if cond else C_TEXT)
        acc = c.step(pat, self.sel_track, self.sel_step, F_ACCENT)
        self.canvas.itemconfigure(self.items["accent_bg"],
                                  fill=C_ORANGE if acc else C_PANEL2,
                                  outline=C_ORANGE if acc else C_PANEL3)
        self.canvas.itemconfigure(self.items["accent_tx"],
                                  text="ACCENT ON" if acc else "ACCENT",
                                  fill="#14100c" if acc else C_TEXT)

        # 打击垫与 pattern 快捷
        for i in range(16):
            sel = (i == self.sel_track)
            self.canvas.itemconfigure(self.items["pad_bg%d" % i],
                                      fill=C_ORANGE2 if sel else C_PANEL2,
                                      outline=C_ORANGE if sel else C_PANEL3)
            self.canvas.itemconfigure(self.items["pad_tx%d" % i],
                                      fill=C_TEXT if sel else C_DIM)
        for i in range(16):
            idx = self.page * 16 + i
            active = (idx == pat)
            self.canvas.itemconfigure(self.items["pq%d_bg" % i],
                                      fill=C_ORANGE if active else C_PANEL2,
                                      outline=C_ORANGE if active else C_PANEL3)
            self.canvas.itemconfigure(self.items["pq%d_tx" % i],
                                      text=str(idx),
                                      fill="#14100c" if active else C_TEXT)

        # 状态
        ts_n, ts_d = c.timesig()
        status = "%s · CPU %d%% · XRUN %d · %s · %s · %d/%d · %s" % (
            self.core.audio_name, int(st.cpuLoad * 100), st.xruns, self.msg,
            "PLAY" if st.playing else "STOP", ts_n, ts_d,
            ("EXPORTING..." if self.export_busy else "READY"))
        self.canvas.itemconfigure(self.items["status_tx"], text=status,
                                  fill=C_DIM if self.core.audio_ok else C_RED)

    def update_slider_value(self, idx, value, text, bipolar):
        key = "ss%d" % idx
        x1, y1, x2, y2 = self.rc[key]
        bx1 = x1 + self.S(78)
        bx2 = x2 - self.S(60)
        by1 = y1 + self.S(7)
        by2 = y2 - self.S(7)
        if bipolar:
            mid = (bx1 + bx2) / 2
            x = bx1 + (bx2 - bx1) * value
            self.canvas.coords(self.items[key + "_fill"], mid, by1, x, by2)
        else:
            x = bx1 + (bx2 - bx1) * value
            self.canvas.coords(self.items[key + "_fill"], bx1, by1, x, by2)
        self.canvas.itemconfigure(self.items[key + "_val"], text=text)

    @staticmethod
    def blend(c1, c2, t):
        a = tuple(int(c1[i:i + 2], 16) for i in (1, 3, 5))
        b = tuple(int(c2[i:i + 2], 16) for i in (1, 3, 5))
        return "#%02x%02x%02x" % tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))

    # ------------------------------------------------------------------
    # 事件
    # ------------------------------------------------------------------
    def hit(self, key, x, y):
        r = self.rc.get(key)
        return bool(r and r[0] <= x <= r[2] and r[1] <= y <= r[3])

    def on_click(self, ev):
        x, y = ev.x, ev.y
        c = self.core
        pat = c.pattern()
        self.dirty = True
        mods = ev.state
        shift = bool(mods & 0x1)
        ctrl = bool(mods & 0x4)
        alt = bool(mods & 0x20000)
        if self.hit("play", x, y):
            if self.core.status().playing:
                c.stop()
            else:
                c.play()
            return
        if self.hit("stop", x, y):
            c.stop_reset()
            return
        if self.hit("tap", x, y):
            self.tap_tempo()
            return
        if self.hit("bpm_minus", x, y):
            c.set_bpm(c.bpm() - 1)
            return
        if self.hit("bpm_plus", x, y):
            c.set_bpm(c.bpm() + 1)
            return
        if self.hit("bpm", x, y):
            self.drag = ("bpm", y, c.bpm())
            return
        if self.hit("swing", x, y):
            r = self.rc["swing"]
            v = int((x - (r[0] + self.S(54))) / max(1, (r[2] - r[0] - self.S(54))) * 100)
            c.set_swing(pat, clamp(v, 0, 100))
            return
        if self.hit("pat_prev", x, y):
            c.select((pat + PATTERNS - 1) % PATTERNS)
            return
        if self.hit("pat_next", x, y):
            c.select((pat + 1) % PATTERNS)
            return
        if self.hit("len_minus", x, y):
            c.set_pattern_len(pat, c.pattern_len(pat) - 1)
            return
        if self.hit("len_plus", x, y):
            c.set_pattern_len(pat, c.pattern_len(pat) + 1)
            return
        if self.hit("song", x, y):
            c.song_set(not c.song_on())
            return
        if self.hit("song_edit", x, y):
            self.song_editor()
            return
        if self.hit("save", x, y):
            self.save_project()
            return
        if self.hit("load", x, y):
            self.load_project()
            return
        if self.hit("wav", x, y):
            self.export(0)
            return
        if self.hit("midi", x, y):
            self.export(1)
            return
        if self.hit("stems", x, y):
            self.export(2)
            return
        if self.hit("clear", x, y):
            self.core.lib.c808_clear_pattern(c.engine, pat)
            return
        if self.hit("copy", x, y):
            dst = (pat + 1) % PATTERNS
            self.core.lib.c808_copy_pattern(c.engine, pat, dst)
            c.select(dst)
            self.msg = "COPIED %d -> %d" % (pat, dst)
            return
        if self.hit("midi_in", x, y):
            self.cycle_midi()
            return
        if self.hit("sync", x, y):
            c.set_clock(not c.clock_source())
            return
        if self.hit("timesig", x, y):
            sigs = [(4, 4), (3, 4), (5, 4), (7, 8), (6, 8), (12, 8)]
            cur = c.timesig()
            nxt = sigs[(sigs.index(cur) + 1) % len(sigs)] if cur in sigs else sigs[0]
            c.set_timesig(nxt[0], nxt[1])
            return
        for i in range(16):
            if self.hit("pq%d" % i, x, y):
                c.select(self.page * 16 + i)
                return
        if self.hit("page_prev", x, y):
            self.page = max(0, self.page - 1)
            return
        if self.hit("page_next", x, y):
            self.page = min(PATTERNS // 16 - 1, self.page + 1)
            return
        for i in range(6):
            if self.hit("vk%d" % i, x, y):
                r = self.rc["vk%d" % i]
                v = clamp((r[3] - self.S(6) - y) / max(1, (r[3] - r[1] - self.S(20))), 0, 1)
                c.set_track(self.sel_track, i, v)
                self.drag = ("vk", i, y, v)
                return
        for i in range(6):
            if self.hit("mk%d" % i, x, y):
                r = self.rc["mk%d" % i]
                v = clamp((r[3] - self.S(6) - y) / max(1, (r[3] - r[1] - self.S(20))), 0, 1)
                c.set_master(i, v)
                self.drag = ("mk", i, y, v)
                return
        for i in range(5):
            if self.hit("ss%d" % i, x, y):
                self.drag = ("ss", i, x, 0)
                self.set_step_slider(i, x)
                return
        if self.hit("cond", x, y):
            cond = (c.step(pat, self.sel_track, self.sel_step, F_COND) + 1) % 7
            cond_n = c.step(pat, self.sel_track, self.sel_step, F_CONDN) or 2
            c.set_cond(pat, self.sel_track, self.sel_step, cond, cond_n)
            return
        if self.hit("accent", x, y):
            acc = c.step(pat, self.sel_track, self.sel_step, F_ACCENT)
            c.set_step(pat, self.sel_track, self.sel_step, F_ACCENT, 0 if acc else 1)
            return
        if self.hit("trklen_m", x, y):
            c.set_track_len(self.sel_track, c.track_len(self.sel_track) - 1)
            return
        if self.hit("trklen_p", x, y):
            c.set_track_len(self.sel_track, c.track_len(self.sel_track) + 1)
            return
        if self.hit("trigger", x, y):
            c.note_on(self.sel_track)
            return
        for i in range(16):
            if self.hit("pad%d" % i, x, y):
                c.note_on(i)
                self.sel_track = i
                return
        for t in range(16):
            if self.hit("mute%d" % t, x, y):
                c.set_mute(t, 0 if c.mute(t) else 1)
                return
            if self.hit("solo%d" % t, x, y):
                c.set_solo(t, 0 if c.solo(t) else 1)
                return
        # 网格
        sx1, sy1, sx2, sy2 = self.rc["grid_steps"]
        if sx1 <= x <= sx2 and sy1 <= y <= sy2:
            ln = clamp(c.pattern_len(pat), 1, STEPS)
            row_h = (sy2 - sy1) / 16.0
            col_w = (sx2 - sx1) / ln
            t = int((y - sy1) / row_h)
            st = int((x - sx1) / col_w)
            if 0 <= t < 16 and 0 <= st < ln:
                self.sel_track = t
                self.sel_step = st
                if ctrl:
                    acc = c.step(pat, t, st, F_ACCENT)
                    c.set_step(pat, t, st, F_ACCENT, 0 if acc else 1)
                elif shift:
                    self.cycle_prob(t, st)
                elif alt:
                    rat = c.step(pat, t, st, F_RATCHET) + 1
                    if rat > 8:
                        rat = 1
                    c.set_step(pat, t, st, F_RATCHET, rat)
                else:
                    on = c.step(pat, t, st, F_ON)
                    c.set_step(pat, t, st, F_ON, 0 if on else 1)

    def on_drag(self, ev):
        if not self.drag:
            return
        kind = self.drag[0]
        if kind == "bpm":
            _, y0, start = self.drag
            delta = (y0 - ev.y) / max(1, self.S(110))
            self.core.set_bpm(clamp(start + delta * 120.0, 20, 400))
        elif kind == "vk":
            _, idx, y0, start = self.drag
            v = clamp(start + (y0 - ev.y) / max(1, self.S(110)), 0, 1)
            self.core.set_track(self.sel_track, idx, v)
        elif kind == "mk":
            _, idx, y0, start = self.drag
            v = clamp(start + (y0 - ev.y) / max(1, self.S(110)), 0, 1)
            self.core.set_master(idx, v)
        elif kind == "ss":
            self.set_step_slider(self.drag[1], ev.x)

    def on_release(self, _ev):
        self.drag = None

    def on_wheel(self, ev):
        x, y = ev.x, ev.y
        c = self.core
        pat = c.pattern()
        self.dirty = True
        if self.hit("bpm", x, y):
            c.set_bpm(c.bpm() + (1 if ev.delta > 0 else -1))
            return
        sx1, sy1, sx2, sy2 = self.rc["grid_steps"]
        if sx1 <= x <= sx2 and sy1 <= y <= sy2:
            ln = clamp(c.pattern_len(pat), 1, STEPS)
            row_h = (sy2 - sy1) / 16.0
            col_w = (sx2 - sx1) / ln
            t = int((y - sy1) / row_h)
            st = int((x - sx1) / col_w)
            if 0 <= t < 16 and 0 <= st < ln:
                v = c.step(pat, t, st, F_VEL) + (4 if ev.delta > 0 else -4)
                c.set_step(pat, t, st, F_VEL, clamp(v, 1, 127))

    def on_key(self, ev):
        key = ev.keysym
        if key == "space":
            if self.core.status().playing:
                self.core.stop()
            else:
                self.core.play()
            return
        if key == "Left":
            self.core.select((self.core.pattern() + PATTERNS - 1) % PATTERNS)
            return
        if key == "Right":
            self.core.select((self.core.pattern() + 1) % PATTERNS)
            return
        if key.lower() == "t":
            self.tap_tempo()
            return
        mapping = {"1": 0, "2": 1, "3": 2, "4": 3, "q": 4, "w": 5, "e": 6, "r": 7,
                   "a": 8, "s": 9, "d": 10, "f": 11, "z": 12, "x": 13, "c": 14, "v": 15}
        k = key.lower()
        if k in mapping:
            self.core.note_on(mapping[k])
            self.sel_track = mapping[k]

    def set_step_slider(self, idx, x):
        x1, y1, x2, y2 = self.rc["ss%d" % idx]
        bx1 = x1 + self.S(78)
        bx2 = x2 - self.S(60)
        t = clamp((x - bx1) / max(1, bx2 - bx1), 0, 1)
        pat = self.core.pattern()
        if idx == 0:
            self.core.set_step(pat, self.sel_track, self.sel_step, F_VEL, int(1 + t * 126))
        elif idx == 1:
            self.core.set_step(pat, self.sel_track, self.sel_step, F_PROB, int(t * 100))
        elif idx == 2:
            self.core.set_step(pat, self.sel_track, self.sel_step, F_RATCHET, int(1 + t * 7))
        elif idx == 3:
            self.core.set_step(pat, self.sel_track, self.sel_step, F_RATE, int(10 + t * 90))
        else:
            self.core.set_step(pat, self.sel_track, self.sel_step, F_MICRO, int((t - 0.5) * 4800))

    def cycle_prob(self, t, st):
        seq = [100, 75, 50, 25, 10, 0]
        cur = self.core.step(self.core.pattern(), t, st, F_PROB)
        try:
            nxt = seq[(seq.index(cur) + 1) % len(seq)]
        except ValueError:
            nxt = 100
        self.core.set_step(self.core.pattern(), t, st, F_PROB, nxt)

    def tap_tempo(self):
        import time
        now = time.monotonic()
        if not hasattr(self, "_taps"):
            self._taps = []
        if self._taps and now - self._taps[-1] > 2.0:
            self._taps = []
        self._taps.append(now)
        if len(self._taps) > 8:
            self._taps.pop(0)
        if len(self._taps) >= 2:
            ds = [self._taps[i + 1] - self._taps[i] for i in range(len(self._taps) - 1)]
            ds = [d for d in ds if 0.15 < d < 1.5]
            if ds:
                bpm = 60.0 / (sum(ds) / len(ds))
                self.core.set_bpm(clamp(bpm, 20, 400))
                self.msg = "TAP %.1f BPM" % bpm

    # ------------------------------------------------------------------
    # 文件 / MIDI / Song / 导出
    # ------------------------------------------------------------------
    def save_project(self):
        path = filedialog.asksaveasfilename(defaultextension=".808",
                                            filetypes=[("808 Project", "*.808")])
        if not path:
            return
        rc = self.core.save(path)
        self.msg = ("SAVED: " + path) if rc == 0 else ("SAVE FAILED %d" % rc)

    def load_project(self):
        path = filedialog.askopenfilename(filetypes=[("808 Project", "*.808")])
        if not path:
            return
        rc = self.core.load(path)
        self.msg = ("LOADED: " + path) if rc == 0 else ("LOAD FAILED %d" % rc)
        self.rebuild()

    def export(self, kind):
        if self.export_busy:
            self.msg = "EXPORT ALREADY RUNNING"
            return
        if kind == 1:
            path = filedialog.asksaveasfilename(defaultextension=".mid",
                                                filetypes=[("MIDI", "*.mid")])
        else:
            path = filedialog.asksaveasfilename(defaultextension=".wav",
                                                filetypes=[("WAV", "*.wav")])
        if not path:
            return
        mode = 1 if self.core.song_on() else 0
        self.export_busy = True
        self.msg = "EXPORTING..."

        def worker():
            if kind == 1:
                rc = self.core.export_midi(path, mode, 2)
            else:
                rc = self.core.export_wav(path, mode, 2, 2, 1 if kind == 2 else 0)
            self.root.after(0, lambda: self.finish_export(rc))

        threading.Thread(target=worker, daemon=True).start()

    def finish_export(self, rc):
        self.export_busy = False
        self.msg = "EXPORT COMPLETE" if rc == 0 else ("EXPORT FAILED %d" % rc)

    def midi_init(self):
        n = self.core.lib.c808_midi_device_count()
        if n > 0:
            if self.core.lib.c808_midi_start(self.core.engine, 0) == 0:
                buf = ctypes.create_string_buffer(256)
                self.core.lib.c808_midi_device_name(0, buf, 256)
                self.midi_name = "MIDI: " + buf.value.decode("utf-8", "replace")
                self.midi_index = 0

    def cycle_midi(self):
        n = self.core.lib.c808_midi_device_count()
        if n <= 0:
            self.midi_name = "MIDI: NONE"
            self.msg = "NO MIDI INPUT DEVICE"
            return
        self.midi_index += 1
        if self.midi_index >= n:
            self.midi_index = -1
            self.core.lib.c808_midi_stop(self.core.engine)
            self.midi_name = "MIDI: OFF"
            self.msg = "MIDI INPUT DISABLED"
            return
        if self.core.lib.c808_midi_start(self.core.engine, self.midi_index) == 0:
            buf = ctypes.create_string_buffer(256)
            self.core.lib.c808_midi_device_name(self.midi_index, buf, 256)
            self.midi_name = "MIDI: " + buf.value.decode("utf-8", "replace")
            self.msg = "MIDI INPUT: " + buf.value.decode("utf-8", "replace")
        else:
            self.midi_name = "MIDI: ERROR"

    def song_editor(self):
        win = tk.Toplevel(self.root)
        win.title("SONG CHAIN EDITOR")
        win.configure(bg=C_PANEL)
        win.geometry("560x520")
        items = []
        for i in range(self.core.song_len()):
            items.append(list(self.core.song_entry(i)))
        listbox = tk.Listbox(win, bg=C_PANEL2, fg=C_TEXT, selectbackground=C_ORANGE,
                             highlightthickness=0, font=("Consolas", 11))
        listbox.pack(fill="both", expand=True, padx=12, pady=12)

        def refresh():
            listbox.delete(0, tk.END)
            for i, (p, r) in enumerate(items):
                listbox.insert(tk.END, "%02d   PATTERN %03d   x%d" % (i + 1, p, r))

        def cmd(kind):
            sel = listbox.curselection()
            idx = sel[0] if sel else -1
            if kind == "add":
                items.append([items[-1][0] if items else 0, 4])
            elif kind == "del" and idx >= 0:
                items.pop(idx)
            elif kind == "up" and idx > 0:
                items[idx], items[idx - 1] = items[idx - 1], items[idx]
            elif kind == "down" and 0 <= idx < len(items) - 1:
                items[idx], items[idx + 1] = items[idx + 1], items[idx]
            elif kind == "pat-" and idx >= 0:
                items[idx][0] = (items[idx][0] - 1) % PATTERNS
            elif kind == "pat+" and idx >= 0:
                items[idx][0] = (items[idx][0] + 1) % PATTERNS
            elif kind == "rep-" and idx >= 0:
                items[idx][1] = max(1, items[idx][1] - 1)
            elif kind == "rep+" and idx >= 0:
                items[idx][1] = min(64, items[idx][1] + 1)
            refresh()

        bar = tk.Frame(win, bg=C_PANEL)
        bar.pack(fill="x", padx=12)
        for label, kind in [("+ ADD", "add"), ("- DEL", "del"), ("UP", "up"), ("DOWN", "down"),
                            ("PAT -", "pat-"), ("PAT +", "pat+"), ("REP -", "rep-"), ("REP +", "rep+")]:
            tk.Button(bar, text=label, command=lambda k=kind: cmd(k),
                      bg=C_PANEL3, fg=C_TEXT, relief="flat").pack(side="left", padx=2, pady=4)

        def apply():
            self.core.lib.c808_set_song_len(self.core.engine, len(items))
            for i, (p, r) in enumerate(items):
                self.core.lib.c808_set_song_entry(self.core.engine, i, p, r)
            win.destroy()

        btns = tk.Frame(win, bg=C_PANEL)
        btns.pack(fill="x", padx=12, pady=10)
        tk.Button(btns, text="APPLY & CLOSE", command=apply,
                  bg=C_ORANGE, fg="#14100c", relief="flat").pack(side="left", padx=4)
        tk.Button(btns, text="CANCEL", command=win.destroy,
                  bg=C_PANEL3, fg=C_TEXT, relief="flat").pack(side="left", padx=4)
        refresh()

    def on_close(self):
        try:
            self.core.close()
        finally:
            self.root.destroy()


def main():
    core = Core()
    if not core.open():
        root = tk.Tk()
        messagebox.showerror("808", "音频引擎初始化失败，将以静音模式运行。")
        root.destroy()
    root = tk.Tk()
    app = App(root, core)
    if "--autoplay" in sys.argv:
        root.after(1200, core.play)
    root.mainloop()
    core.close()


if __name__ == "__main__":
    main()
