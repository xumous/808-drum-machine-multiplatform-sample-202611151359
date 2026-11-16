# -*- coding: utf-8 -*-
"""
Xu808 图形界面 —— PySide6 实现。

徐某在布局上刻意模仿 TR-808 的“深色金属面板 + 橙色按钮”：
  · 上方是 Logo 与走带控制；
  · 中间是 16 轨 × N 步的网格，左键开关、右键编辑步进属性；
  · 下方是 16 条通道条（Tune/Decay/Tone/Level/Pan/M/S/轨道长度）；
  · 最下面是主效果与电平表。

GUI 线程只做两件事：把用户操作写进 engine.project，和定时读取 engine.snapshot()。
它绝不会去等待音频线程，因此拖动旋钮、打开文件对话框都不会让鼓点卡顿。
"""

from __future__ import annotations

import os
import time
from typing import Dict, List, Optional

from PySide6.QtCore import Qt, QTimer, Signal, QRectF, QSize, QPointF
from PySide6.QtGui import (QAction, QColor, QFont, QIcon, QLinearGradient,
                           QPainter, QPen, QBrush, QPixmap, QPolygonF)
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog,
                               QDialogButtonBox, QDoubleSpinBox, QFileDialog,
                               QFrame, QGridLayout, QGroupBox, QHBoxLayout,
                               QLabel, QMainWindow, QMenu, QMessageBox,
                               QPushButton, QScrollArea, QSizePolicy, QSlider,
                               QSpinBox, QTableWidget, QTableWidgetItem,
                               QVBoxLayout, QWidget, QHeaderView, QInputDialog)

from .dsp import PAD_CODES, PAD_COUNT, PAD_NAMES_CN, PAD_MIDI_NOTES
from .engine import XuEngine, export_midi
from .project import (COND_NAMES_CN, XuPattern, XuProject, XuSongEntry)

# ---------------------------------------------------------------------------
# 颜色常量：整套 UI 只允许用这些颜色，保证视觉统一
# ---------------------------------------------------------------------------
C_BG = "#121212"  # 窗口底色，接近 808 的黑色机身
C_PANEL = "#1c1c1c"  # 面板
C_PANEL2 = "#232323"  # 次级面板
C_LINE = "#2e2e2e"  # 分隔线
C_ORANGE = "#ff7a18"  # 808 的招牌橙色
C_ORANGE_D = "#a84a0c"  # 暗橙（按下状态）
C_YELLOW = "#ffd166"  # 重音
C_CYAN = "#4cc9f0"  # 播放头
C_TEXT = "#e8e8e8"  # 主文字
C_DIM = "#8a8a8a"  # 次要文字
C_RED = "#e5484d"  # 静音/过载警告

# 每条通道条显示哪些参数（键名对应 XuPadParams 的字段）
STRIP_PARAMS: Dict[int, List[tuple]] = {
    0: [("TUNE", "tune"), ("DECAY", "decay"), ("TONE", "tone")],
    1: [("TUNE", "tune"), ("SNAP", "snappy"), ("DECAY", "decay")],
    2: [("TUNE", "tune"), ("DECAY", "decay"), ("COLOR", "tone")],
    3: [("TUNE", "tune"), ("DECAY", "decay"), ("COLOR", "tone")],
    4: [("TUNE", "tune"), ("DECAY", "decay"), ("COLOR", "tone")],
    5: [("TUNE", "tune"), ("DECAY", "decay")],
    6: [("TUNE", "tune"), ("DECAY", "decay")],
    7: [("TUNE", "tune"), ("DECAY", "decay")],
    8: [("TUNE", "tune"), ("DECAY", "decay")],
    9: [("TUNE", "tune"), ("DECAY", "decay")],
    10: [("DECAY", "decay"), ("TONE", "tone"), ("TAIL", "tail")],
    11: [("DECAY", "decay"), ("TONE", "tone")],
    12: [("TUNE", "tune"), ("DECAY", "decay")],
    13: [("DECAY", "decay"), ("TONE", "tone")],
    14: [("DECAY", "decay"), ("TONE", "tone")],
    15: [("DECAY", "decay"), ("TONE", "tone")],
}

# 全局 QSS：把滑块、按钮、下拉框全部改成深色 + 橙色，避免 Windows 原生白底破坏氛围
QSS = f"""
QWidget {{ background: {C_BG}; color: {C_TEXT}; font-family: 'Microsoft YaHei', 'Segoe UI'; font-size: 12px; }}
QGroupBox {{ border: 1px solid {C_LINE}; border-radius: 6px; margin-top: 8px; padding-top: 6px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 8px; color: {C_DIM}; }}
QPushButton {{ background: {C_PANEL2}; border: 1px solid {C_LINE}; border-radius: 5px; padding: 4px 8px; }}
QPushButton:hover {{ border-color: {C_ORANGE}; }}
QPushButton:pressed {{ background: {C_ORANGE_D}; }}
QPushButton:checked {{ background: {C_ORANGE}; color: #1a1008; border-color: {C_ORANGE}; }}
QComboBox, QSpinBox, QDoubleSpinBox {{ background: {C_PANEL2}; border: 1px solid {C_LINE}; border-radius: 4px; padding: 2px 4px; }}
QComboBox QAbstractItemView {{ background: {C_PANEL2}; selection-background-color: {C_ORANGE_D}; }}
QSlider::groove:vertical {{ width: 6px; background: #101010; border-radius: 3px; }}
QSlider::handle:vertical {{ height: 14px; margin: 0 -5px; border-radius: 3px; background: {C_ORANGE}; }}
QSlider::sub-page:vertical {{ background: {C_ORANGE_D}; border-radius: 3px; }}
QSlider::groove:horizontal {{ height: 6px; background: #101010; border-radius: 3px; }}
QSlider::handle:horizontal {{ width: 14px; margin: -5px 0; border-radius: 3px; background: {C_ORANGE}; }}
QSlider::sub-page:horizontal {{ background: {C_ORANGE_D}; border-radius: 3px; }}
QScrollArea {{ border: none; }}
QTableWidget {{ background: {C_PANEL2}; gridline-color: {C_LINE}; }}
QHeaderView::section {{ background: {C_PANEL}; color: {C_DIM}; border: none; padding: 3px; }}
QMenuBar {{ background: {C_PANEL}; }}
QMenuBar::item:selected {{ background: {C_ORANGE_D}; }}
QMenu {{ background: {C_PANEL2}; border: 1px solid {C_LINE}; }}
QMenu::item:selected {{ background: {C_ORANGE_D}; }}
QStatusBar {{ background: {C_PANEL}; color: {C_DIM}; }}
"""


class XuMeter(QWidget):
    """双声道电平表：绿色→黄色→红色渐变，带峰值保持。"""

    def __init__(self) -> None:
        super().__init__()
        self.setFixedSize(150, 22)
        self.l = 0.0
        self.r = 0.0

    def set_levels(self, l: float, r: float) -> None:
        self.l = min(1.0, max(0.0, l))
        self.r = min(1.0, max(0.0, r))
        self.update()

    def paintEvent(self, ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), QColor("#0c0c0c"))
        w = self.width() - 4
        for i, v in enumerate((self.l, self.r)):
            y = 3 + i * 9
            p.fillRect(2, y, w, 7, QColor("#1a1a1a"))
            # 电平条：分三段着色，超过 -3dB 变红
            seg = int(w * v)
            if seg > 0:
                grad = QLinearGradient(2, 0, 2 + w, 0)
                grad.setColorAt(0.0, QColor("#3ddc84"))
                grad.setColorAt(0.72, QColor("#ffd166"))
                grad.setColorAt(0.95, QColor("#e5484d"))
                p.fillRect(2, y, seg, 7, QBrush(grad))
        p.setPen(QPen(QColor(C_LINE)))
        p.drawRect(1, 2, self.width() - 2, self.height() - 4)


class XuGrid(QWidget):
    """
    步进网格：整个程序的“乐谱”。

    徐某没有用 QTableWidget，因为 16×64 个单元格的 item 开销太大，
    而且很难画出播放头光晕；自绘一个 QWidget 反而更轻、更快、更好看。
    """

    hitPad = Signal(int)  # 点击左侧名字 → 试听
    toggleStep = Signal(int, int)  # 左键格子 → 开关
    editStep = Signal(int, int)  # 右键格子 → 打开属性
    toggleMute = Signal(int)  # 点击 M
    toggleSolo = Signal(int)  # 点击 S

    CELL_W = 32
    CELL_H = 22
    LABEL_W = 132
    HEADER_H = 24

    def __init__(self, project: XuProject) -> None:
        super().__init__()
        self.project = project
        self.playhead = -1  # 当前播放步，-1 表示停止
        self.cols = project.patterns[project.current_pattern].steps
        self.setMouseTracking(True)
        self._rebuild_size()

    def _pattern(self) -> XuPattern:
        return self.project.patterns[self.project.current_pattern]

    def _rebuild_size(self) -> None:
        """根据步数计算控件大小；放在滚动区里就能横向滚动。"""
        self.cols = max(1, self._pattern().steps)
        w = self.LABEL_W + self.cols * self.CELL_W
        h = self.HEADER_H + PAD_COUNT * self.CELL_H
        self.setFixedSize(w, h)
        self.update()

    def refresh(self) -> None:
        """工程/pattern 改变后由主窗口调用。"""
        self._rebuild_size()

    def set_playhead(self, step: int) -> None:
        if step != self.playhead:
            self.playhead = step
            self.update()

    # ------------------------------------------------------------------
    def paintEvent(self, ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        pat = self._pattern()
        # 背景
        p.fillRect(self.rect(), QColor(C_BG))
        # 顶部步号条
        p.fillRect(0, 0, self.width(), self.HEADER_H, QColor(C_PANEL))
        p.setFont(QFont("Segoe UI", 8))
        # 每 4 步加一条亮分隔线，模拟 808 的“拍点”
        for c in range(self.cols):
            x = self.LABEL_W + c * self.CELL_W
            if c % 4 == 0:
                p.fillRect(x, 0, 1, self.height(), QColor("#3a3a3a"))
            p.setPen(QPen(QColor(C_DIM)))
            p.drawText(QRectF(x, 0, self.CELL_W, self.HEADER_H),
                       Qt.AlignmentFlag.AlignCenter, str(c + 1))
        # 声部标签区
        p.fillRect(0, self.HEADER_H, self.LABEL_W, self.height() - self.HEADER_H, QColor(C_PANEL2))
        for r in range(PAD_COUNT):
            y = self.HEADER_H + r * self.CELL_H
            # 行底色：奇偶交替，眼睛不容易串行
            p.fillRect(self.LABEL_W, y, self.width() - self.LABEL_W, self.CELL_H,
                       QColor("#161616" if r % 2 == 0 else "#1a1a1a"))
            # 声部代号（橙色）+ 中文名（灰色）
            p.setFont(QFont("Consolas", 9, QFont.Weight.Bold))
            p.setPen(QPen(QColor(C_ORANGE)))
            p.drawText(QRectF(6, y, 36, self.CELL_H),
                       Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, PAD_CODES[r])
            p.setFont(QFont("Microsoft YaHei", 8))
            p.setPen(QPen(QColor(C_TEXT)))
            p.drawText(QRectF(42, y, 46, self.CELL_H),
                       Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, PAD_NAMES_CN[r])
            # M / S 小方块
            pad = self.project.pads[r]
            for j, (txt, on, col) in enumerate((("M", pad.mute, C_RED), ("S", pad.solo, C_YELLOW))):
                bx = 92 + j * 18
                by = y + 4
                p.fillRect(bx, by, 15, self.CELL_H - 8,
                           QColor(col if on else "#2a2a2a"))
                p.setPen(QPen(QColor("#121212" if on else C_DIM)))
                p.setFont(QFont("Segoe UI", 7, QFont.Weight.Bold))
                p.drawText(QRectF(bx, by, 15, self.CELL_H - 8),
                           Qt.AlignmentFlag.AlignCenter, txt)
            # 底部行线
            p.setPen(QPen(QColor(C_LINE)))
            p.drawLine(0, y + self.CELL_H - 1, self.width(), y + self.CELL_H - 1)
        # 格子
        for c in range(self.cols):
            x = self.LABEL_W + c * self.CELL_W
            for r in range(PAD_COUNT):
                st = pat.data[r][c]
                y = self.HEADER_H + r * self.CELL_H
                cell = QRectF(x + 2, y + 2, self.CELL_W - 4, self.CELL_H - 4)
                if st.on:
                    # 力度越大颜色越亮；重音用黄色，和普通橙色区分
                    base = QColor(C_YELLOW if st.accent else C_ORANGE)
                    k = 0.42 + 0.58 * (st.velocity / 127.0)
                    base.setAlphaF(min(1.0, k))
                    p.fillRect(cell, base)
                    # 概率/ratchet 等小字用深色，保证在亮块上可读
                    if st.prob < 100 or st.ratchet > 1 or st.micro != 0:
                        p.setPen(QPen(QColor("#1a1008")))
                        p.setFont(QFont("Consolas", 6, QFont.Weight.Bold))
                        txt = st.key().replace(" ", "")
                        p.drawText(cell, Qt.AlignmentFlag.AlignCenter, txt[:6])
                else:
                    p.fillRect(cell, QColor("#101010"))
                    p.setPen(QPen(QColor("#272727")))
                    p.drawRect(cell)
        # 播放头：整列青色描边 + 顶部小三角
        if 0 <= self.playhead < self.cols:
            x = self.LABEL_W + self.playhead * self.CELL_W
            p.fillRect(x, self.HEADER_H, self.CELL_W, self.height() - self.HEADER_H,
                       QColor(76, 201, 240, 26))
            p.setPen(QPen(QColor(C_CYAN), 2))
            p.drawLine(x, self.HEADER_H, x, self.height())
            p.drawLine(x + self.CELL_W, self.HEADER_H, x + self.CELL_W, self.height())
            tri = QColor(C_CYAN)
            p.setBrush(QBrush(tri))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawPolygon(QPolygonF([QPointF(x + 8, 0), QPointF(x + self.CELL_W - 8, 0),
                                     QPointF(x + self.CELL_W // 2, 10)]))
        # 外框
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor(C_LINE)))
        p.drawRect(self.rect().adjusted(0, 0, -1, -1))

    @staticmethod
    def _pt(x: float, y: float):
        return QPointF(x, y)

    # ------------------------------------------------------------------
    def mousePressEvent(self, ev) -> None:
        pos = ev.position()
        x, y = pos.x(), pos.y()
        if y < self.HEADER_H:
            return
        r = int((y - self.HEADER_H) // self.CELL_H)
        if not (0 <= r < PAD_COUNT):
            return
        if x < self.LABEL_W:
            # 左侧：M / S / 试听
            for j, sig in enumerate((self.toggleMute, self.toggleSolo)):
                bx = 92 + j * 18
                if bx <= x <= bx + 15:
                    sig.emit(r)
                    return
            self.hitPad.emit(r)
            return
        c = int((x - self.LABEL_W) // self.CELL_W)
        if not (0 <= c < self.cols):
            return
        if ev.button() == Qt.MouseButton.RightButton:
            self.editStep.emit(r, c)
        else:
            self.toggleStep.emit(r, c)


class XuChannelStrip(QWidget):
    """一条通道条：名字、M/S、2~3 个音色旋钮、音量、声像、轨道长度。"""

    hit = Signal(int)
    changed = Signal()  # 任何参数变化后通知主窗口（用于标记工程已修改）

    def __init__(self, pad: int, project: XuProject) -> None:
        super().__init__()
        self.pad = pad
        self.project = project
        self.setFixedWidth(74)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(2, 2, 2, 2)
        lay.setSpacing(2)
        # 名字按钮：点一下就试听，比只能拖参数直观得多
        btn = QPushButton(f"{PAD_CODES[pad]}\n{PAD_NAMES_CN[pad]}")
        btn.setFixedHeight(34)
        btn.setStyleSheet(f"QPushButton{{font-size:10px;color:{C_ORANGE};border-color:#3a2a1a;}}")
        btn.clicked.connect(lambda: self.hit.emit(self.pad))
        lay.addWidget(btn)
        # M / S
        row = QHBoxLayout()
        row.setSpacing(2)
        self.bm = QPushButton("M")
        self.bs = QPushButton("S")
        for b, col in ((self.bm, C_RED), (self.bs, C_YELLOW)):
            b.setCheckable(True)
            b.setFixedHeight(16)
            b.setStyleSheet(f"QPushButton{{font-size:9px;padding:0;}}"
                            f"QPushButton:checked{{background:{col};color:#111;}}")
            row.addWidget(b)
        lay.addLayout(row)
        p = project.pads[pad]
        self.bm.setChecked(p.mute)
        self.bs.setChecked(p.solo)
        self.bm.toggled.connect(lambda v: self._set_attr("mute", v))
        self.bs.toggled.connect(lambda v: self._set_attr("solo", v))
        # 音色参数滑块
        for label, attr in STRIP_PARAMS[pad]:
            lay.addWidget(self._mini_label(label))
            s = QSlider(Qt.Orientation.Vertical)
            s.setRange(0, 100)
            s.setValue(int(round(getattr(p, attr) * 100)))
            s.setFixedHeight(38)
            s.setToolTip(f"{label}（{attr}）")
            s.valueChanged.connect(lambda v, a=attr: self._set_attr(a, v / 100.0))
            lay.addWidget(s, 0, Qt.AlignmentFlag.AlignHCenter)
        # 音量
        lay.addWidget(self._mini_label("LEVEL"))
        self.sl_level = QSlider(Qt.Orientation.Vertical)
        self.sl_level.setRange(0, 125)
        self.sl_level.setValue(int(round(p.level * 100)))
        self.sl_level.setFixedHeight(38)
        self.sl_level.valueChanged.connect(lambda v: self._set_attr("level", v / 100.0))
        lay.addWidget(self.sl_level, 0, Qt.AlignmentFlag.AlignHCenter)
        # 声像
        lay.addWidget(self._mini_label("PAN"))
        self.sl_pan = QSlider(Qt.Orientation.Vertical)
        self.sl_pan.setRange(0, 100)
        self.sl_pan.setValue(int(round(p.pan * 100)))
        self.sl_pan.setFixedHeight(30)
        self.sl_pan.valueChanged.connect(lambda v: self._set_attr("pan", v / 100.0))
        lay.addWidget(self.sl_pan, 0, Qt.AlignmentFlag.AlignHCenter)
        # 轨道长度（polymeter）：1..64，默认 16
        self.sp_len = QSpinBox()
        self.sp_len.setRange(1, 64)
        self.sp_len.setValue(project.patterns[project.current_pattern].track_len[pad])
        self.sp_len.setFixedHeight(18)
        self.sp_len.setStyleSheet("QSpinBox{font-size:9px;}")
        self.sp_len.setToolTip("本轨循环长度（polymeter），与其它轨独立")
        self.sp_len.valueChanged.connect(self._set_track_len)
        lay.addWidget(self.sp_len)
        lay.addStretch(1)

    @staticmethod
    def _mini_label(text: str) -> QLabel:
        lab = QLabel(text)
        lab.setAlignment(Qt.AlignmentFlag.AlignCenter)
        lab.setStyleSheet(f"color:{C_DIM};font-size:8px;")
        return lab

    def _set_attr(self, attr: str, value) -> None:
        """写参数；bool 和 float 分开处理，避免 True 被写成 1.0 以外的怪值。"""
        p = self.project.pads[self.pad]
        setattr(p, attr, bool(value) if isinstance(value, bool) else float(value))
        self.changed.emit()

    def _set_track_len(self, v: int) -> None:
        self.project.patterns[self.project.current_pattern].track_len[self.pad] = int(v)
        self.changed.emit()

    def sync_from_project(self) -> None:
        """加载工程后把控件拉回正确位置（不触发信号用 blockSignals）。"""
        p = self.project.pads[self.pad]
        self.bm.blockSignals(True)
        self.bs.blockSignals(True)
        self.bm.setChecked(p.mute)
        self.bs.setChecked(p.solo)
        self.bm.blockSignals(False)
        self.bs.blockSignals(False)
        self.sl_level.blockSignals(True)
        self.sl_level.setValue(int(round(p.level * 100)))
        self.sl_level.blockSignals(False)
        self.sl_pan.blockSignals(True)
        self.sl_pan.setValue(int(round(p.pan * 100)))
        self.sl_pan.blockSignals(False)
        self.sp_len.blockSignals(True)
        self.sp_len.setValue(self.project.patterns[self.project.current_pattern].track_len[self.pad])
        self.sp_len.blockSignals(False)


class XuStepDialog(QDialog):
    """格子的高级属性：力度、重音、概率、微时、ratchet、条件。"""

    def __init__(self, step, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("步进属性 - 徐某的 808")
        self.setModal(True)
        self.step = step
        lay = QVBoxLayout(self)
        form = QGridLayout()
        row = 0

        self.cb_on = QCheckBox("启用")
        self.cb_on.setChecked(step.on)
        form.addWidget(self.cb_on, row, 0, 1, 2)
        row += 1

        self.cb_acc = QCheckBox("重音 Accent（改变音量+衰减+音色）")
        self.cb_acc.setChecked(step.accent)
        form.addWidget(self.cb_acc, row, 0, 1, 2)
        row += 1

        form.addWidget(QLabel("力度 Velocity (1-127)"), row, 0)
        self.sp_vel = QSpinBox()
        self.sp_vel.setRange(1, 127)
        self.sp_vel.setValue(step.velocity)
        form.addWidget(self.sp_vel, row, 1)
        row += 1

        form.addWidget(QLabel("概率 Probability (0-100%)"), row, 0)
        self.sp_prob = QSpinBox()
        self.sp_prob.setRange(0, 100)
        self.sp_prob.setValue(step.prob)
        form.addWidget(self.sp_prob, row, 1)
        row += 1

        form.addWidget(QLabel("微时偏移 Micro (样本, ±2400)"), row, 0)
        self.sp_micro = QSpinBox()
        self.sp_micro.setRange(-2400, 2400)
        self.sp_micro.setValue(step.micro)
        form.addWidget(self.sp_micro, row, 1)
        row += 1

        form.addWidget(QLabel("滚奏 Ratchet (1-8)"), row, 0)
        self.sp_rat = QSpinBox()
        self.sp_rat.setRange(1, 8)
        self.sp_rat.setValue(step.ratchet)
        form.addWidget(self.sp_rat, row, 1)
        row += 1

        form.addWidget(QLabel("条件触发"), row, 0)
        self.cb_cond = QComboBox()
        self.cb_cond.addItems(COND_NAMES_CN)
        self.cb_cond.setCurrentIndex(max(0, min(4, step.cond)))
        form.addWidget(self.cb_cond, row, 1)
        lay.addLayout(form)

        bb = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                              QDialogButtonBox.StandardButton.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)

    def apply(self) -> None:
        """把对话框里的值写回 XuStep。只有按 OK 才会调用。"""
        self.step.on = self.cb_on.isChecked()
        self.step.accent = self.cb_acc.isChecked()
        self.step.velocity = self.sp_vel.value()
        self.step.prob = self.sp_prob.value()
        self.step.micro = self.sp_micro.value()
        self.step.ratchet = self.sp_rat.value()
        self.step.cond = self.cb_cond.currentIndex()


class XuSongDialog(QDialog):
    """歌曲链编辑器：每行一个“pattern + 重复次数”。"""

    def __init__(self, project: XuProject, parent=None) -> None:
        super().__init__(parent)
        self.project = project
        self.setWindowTitle("歌曲链 - XU-808")
        self.resize(460, 320)
        lay = QVBoxLayout(self)
        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(["Pattern", "重复次数"])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        lay.addWidget(self.table)
        for e in project.song.entries or [XuSongEntry(0, 1)]:
            self._add_row(e.pattern, e.repeats)
        row = QHBoxLayout()
        b_add = QPushButton("添加")
        b_del = QPushButton("删除选中")
        b_add.clicked.connect(lambda: self._add_row(0, 1))
        b_del.clicked.connect(self._del_row)
        row.addWidget(b_add)
        row.addWidget(b_del)
        row.addStretch(1)
        lay.addLayout(row)
        bb = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                              QDialogButtonBox.StandardButton.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)

    def _add_row(self, pat: int, rep: int) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        cb = QComboBox()
        for i, p in enumerate(self.project.patterns):
            cb.addItem(f"{i + 1}. {p.name}")
        cb.setCurrentIndex(max(0, min(len(self.project.patterns) - 1, pat)))
        self.table.setCellWidget(r, 0, cb)
        sp = QSpinBox()
        sp.setRange(1, 99)
        sp.setValue(max(1, rep))
        self.table.setCellWidget(r, 1, sp)

    def _del_row(self) -> None:
        r = self.table.currentRow()
        if r >= 0:
            self.table.removeRow(r)

    def apply(self) -> None:
        entries: List[XuSongEntry] = []
        for r in range(self.table.rowCount()):
            cb = self.table.cellWidget(r, 0)
            sp = self.table.cellWidget(r, 1)
            if cb and sp:
                entries.append(XuSongEntry(cb.currentIndex(), sp.value()))
        self.project.song.entries = entries


class XuMainWindow(QMainWindow):
    """主窗口：组装所有部件，并把它们接到引擎上。"""

    def __init__(self, project: XuProject) -> None:
        super().__init__()
        self.project = project
        self.engine = XuEngine(project)
        self.midi = None
        self._dirty = False
        self._tap_times: List[float] = []
        self._audio_ok = False
        self.setWindowTitle("XU-808 鼓机 - 徐某")
        self.resize(1520, 920)
        self.setStyleSheet(QSS)
        self._build_menu()
        self._build_ui()
        self._build_timer()
        self._start_audio()
        self._start_midi()

    # ------------------------------------------------------------------
    # 构建
    # ------------------------------------------------------------------
    def _build_menu(self) -> None:
        m = self.menuBar()
        fm = m.addMenu("文件(&F)")
        self._act(fm, "新建工程", self.on_new)
        self._act(fm, "打开工程…", self.on_load, "Ctrl+O")
        self._act(fm, "保存工程…", self.on_save, "Ctrl+S")
        fm.addSeparator()
        self._act(fm, "导出 WAV（Pattern）…", lambda: self.on_export_wav("pattern"))
        self._act(fm, "导出 WAV（Song）…", lambda: self.on_export_wav("song"))
        self._act(fm, "分轨导出 WAV…", self.on_export_stems)
        self._act(fm, "导出 MIDI…", self.on_export_midi)
        fm.addSeparator()
        self._act(fm, "退出", self.close, "Ctrl+Q")
        hm = m.addMenu("帮助(&H)")
        self._act(hm, "关于 XU-808", self.on_about)

    @staticmethod
    def _act(menu: QMenu, text: str, slot, shortcut: str = "") -> None:
        """创建一个菜单项；菜单项的父对象是菜单本身，这样才能被正确回收。"""
        a = QAction(text, menu)
        if shortcut:
            a.setShortcut(shortcut)
        a.triggered.connect(slot)
        menu.addAction(a)

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 4, 8, 4)
        root.setSpacing(6)

        # ---------------- 顶部走带条 ----------------
        top = QHBoxLayout()
        logo = QLabel("XU-808")
        logo.setStyleSheet(f"color:{C_ORANGE};font-size:26px;font-weight:900;"
                           f"font-family:Consolas;letter-spacing:2px;")
        sub = QLabel("RHYTHM COMPOSER  ·  徐某  ·  16 VOICE ANALOG")
        sub.setStyleSheet(f"color:{C_DIM};font-size:9px;")
        lv = QVBoxLayout()
        lv.setSpacing(0)
        lv.addWidget(logo)
        lv.addWidget(sub)
        top.addLayout(lv)
        top.addSpacing(16)

        self.b_play = QPushButton("▶  PLAY")
        self.b_stop = QPushButton("■  STOP")
        for b in (self.b_play, self.b_stop):
            b.setFixedSize(92, 40)
            b.setStyleSheet("QPushButton{font-weight:bold;font-size:13px;}")
        self.b_play.clicked.connect(self.on_play)
        self.b_stop.clicked.connect(self.on_stop)
        top.addWidget(self.b_play)
        top.addWidget(self.b_stop)

        # BPM
        gb = QGroupBox("BPM")
        gl = QHBoxLayout(gb)
        gl.setContentsMargins(6, 4, 6, 4)
        self.sp_bpm = QDoubleSpinBox()
        self.sp_bpm.setRange(40.0, 300.0)
        self.sp_bpm.setDecimals(1)
        self.sp_bpm.setValue(self.project.master.bpm)
        self.sp_bpm.setFixedWidth(72)
        self.sp_bpm.valueChanged.connect(self._on_bpm)
        self.sl_bpm = QSlider(Qt.Orientation.Horizontal)
        self.sl_bpm.setRange(40, 300)
        self.sl_bpm.setValue(int(self.project.master.bpm))
        self.sl_bpm.setFixedWidth(110)
        self.sl_bpm.valueChanged.connect(lambda v: self.sp_bpm.setValue(float(v)))
        gl.addWidget(self.sp_bpm)
        gl.addWidget(self.sl_bpm)
        top.addWidget(gb)
        b_tap = QPushButton("TAP")
        b_tap.setFixedSize(52, 40)
        b_tap.clicked.connect(self.on_tap)
        top.addWidget(b_tap)

        # Swing
        gs = QGroupBox("SWING")
        gsl = QHBoxLayout(gs)
        gsl.setContentsMargins(6, 4, 6, 4)
        self.sl_swing = QSlider(Qt.Orientation.Horizontal)
        self.sl_swing.setRange(0, 75)
        self.sl_swing.setValue(int(self.project.master.swing * 100))
        self.sl_swing.setFixedWidth(110)
        self.lb_swing = QLabel(f"{self.sl_swing.value()}%")
        self.lb_swing.setFixedWidth(32)
        self.lb_swing.setStyleSheet(f"color:{C_ORANGE};")
        self.sl_swing.valueChanged.connect(self._on_swing)
        gsl.addWidget(self.sl_swing)
        gsl.addWidget(self.lb_swing)
        top.addWidget(gs)

        # Pattern + 步数
        gp = QGroupBox("PATTERN")
        gpl = QHBoxLayout(gp)
        gpl.setContentsMargins(6, 4, 6, 4)
        self.cb_pat = QComboBox()
        for i, p in enumerate(self.project.patterns):
            self.cb_pat.addItem(f"{i + 1:02d} {p.name}")
        self.cb_pat.setCurrentIndex(self.project.current_pattern)
        self.cb_pat.currentIndexChanged.connect(self.on_pattern)
        self.sp_steps = QSpinBox()
        self.sp_steps.setRange(1, 64)
        self.sp_steps.setValue(self.pattern().steps)
        self.sp_steps.setFixedWidth(54)
        self.sp_steps.setToolTip("Pattern 步数（1-64）")
        self.sp_steps.valueChanged.connect(self.on_steps)
        gpl.addWidget(self.cb_pat)
        gpl.addWidget(QLabel("步数"))
        gpl.addWidget(self.sp_steps)
        top.addWidget(gp)

        # 歌曲
        self.cb_song = QPushButton("SONG")
        self.cb_song.setCheckable(True)
        self.cb_song.setChecked(self.project.master.song_mode)
        self.cb_song.setFixedHeight(40)
        self.cb_song.clicked.connect(self.on_song_toggle)
        top.addWidget(self.cb_song)
        b_songedit = QPushButton("歌曲链…")
        b_songedit.setFixedHeight(40)
        b_songedit.clicked.connect(self.on_song_edit)
        top.addWidget(b_songedit)

        top.addStretch(1)
        # 右侧：MIDI 状态与外部时钟
        self.cb_midiclk = QCheckBox("MIDI CLK")
        self.cb_midiclk.setToolTip("勾选后由外部 MIDI 时钟推进音序")
        self.cb_midiclk.toggled.connect(self.on_midi_clock)
        top.addWidget(self.cb_midiclk)
        self.lb_midi = QLabel("MIDI: -")
        self.lb_midi.setStyleSheet(f"color:{C_DIM};font-size:10px;")
        top.addWidget(self.lb_midi)
        root.addLayout(top)

        # ---------------- 网格 ----------------
        self.grid = XuGrid(self.project)
        self.grid.hitPad.connect(self.on_hit_pad)
        self.grid.toggleStep.connect(self.on_toggle_step)
        self.grid.editStep.connect(self.on_edit_step)
        self.grid.toggleMute.connect(self.on_toggle_mute)
        self.grid.toggleSolo.connect(self.on_toggle_solo)
        scroll = QScrollArea()
        scroll.setWidgetResizable(False)
        scroll.setWidget(self.grid)
        scroll.setFixedHeight(XuGrid.HEADER_H + PAD_COUNT * XuGrid.CELL_H + 18)
        root.addWidget(scroll)

        # ---------------- 通道条 ----------------
        ch_scroll = QScrollArea()
        ch_scroll.setWidgetResizable(True)
        ch_scroll.setFixedHeight(322)
        holder = QWidget()
        hl = QHBoxLayout(holder)
        hl.setContentsMargins(0, 0, 0, 0)
        hl.setSpacing(2)
        self.strips: List[XuChannelStrip] = []
        for p in range(PAD_COUNT):
            s = XuChannelStrip(p, self.project)
            s.hit.connect(self.on_hit_pad)
            s.changed.connect(self._mark_dirty)
            hl.addWidget(s)
            self.strips.append(s)
        hl.addStretch(1)
        ch_scroll.setWidget(holder)
        root.addWidget(ch_scroll)

        # ---------------- 主效果 ----------------
        mg = QGroupBox("MASTER  ·  徐某的母线")
        ml = QHBoxLayout(mg)
        ml.setContentsMargins(8, 4, 8, 4)
        self.sl_lp = self._master_slider(ml, "LP 低通", 200, 20000,
                                         int(self.project.master.lp_cutoff), self._on_lp)
        self.sl_lpq = self._master_slider(ml, "RES 共鸣", 50, 800,
                                          int(self.project.master.lp_q * 100), self._on_lpq)
        self.sl_hp = self._master_slider(ml, "HP 高通", 10, 800,
                                         int(self.project.master.hp_cutoff), self._on_hp)
        self.sl_drive = self._master_slider(ml, "DRIVE 过载", 100, 800,
                                            int(self.project.master.drive * 100), self._on_drive)
        self.sl_vol = self._master_slider(ml, "VOL 音量", 0, 100,
                                          int(self.project.master.volume * 100), self._on_vol)
        self.meter = XuMeter()
        ml.addWidget(self.meter)
        self.lb_stat = QLabel("CPU 0%  ·  XRUN 0")
        self.lb_stat.setStyleSheet(f"color:{C_DIM};font-size:10px;")
        ml.addWidget(self.lb_stat)
        root.addWidget(mg)

        # 状态栏
        self.statusBar().showMessage("就绪 · 空格键播放/停止 · 左键画格子 · 右键编辑步进")

    def _master_slider(self, lay: QHBoxLayout, text: str, lo: int, hi: int,
                       val: int, slot) -> QSlider:
        """生成一个“标签+滑杆+数值”的小组，主效果区重复用到。"""
        box = QVBoxLayout()
        box.setSpacing(1)
        lab = QLabel(text)
        lab.setStyleSheet(f"color:{C_DIM};font-size:9px;")
        s = QSlider(Qt.Orientation.Horizontal)
        s.setRange(lo, hi)
        s.setValue(max(lo, min(hi, val)))
        s.setFixedWidth(120)
        s.valueChanged.connect(slot)
        box.addWidget(lab)
        box.addWidget(s)
        lay.addLayout(box)
        return s

    def _build_timer(self) -> None:
        """33ms ≈ 30fps，仅刷新 UI；绝不作为音序时钟。"""
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.on_tick)
        self.timer.start(33)

    # ------------------------------------------------------------------
    # 便捷访问
    # ------------------------------------------------------------------
    def pattern(self) -> XuPattern:
        return self.project.patterns[self.project.current_pattern]

    def _mark_dirty(self) -> None:
        self._dirty = True

    def _start_audio(self) -> None:
        try:
            self.engine.start_audio()
            self._audio_ok = True
            self.statusBar().showMessage("音频已启动 · 48000Hz / 512 样本 · PortAudio")
        except Exception as e:
            self._audio_ok = False
            QMessageBox.warning(self, "音频设备", f"无法打开音频设备：\n{e}\n\n"
                                                  "界面仍可使用，离线导出 WAV 不受影响。")

    def _start_midi(self) -> None:
        try:
            from .winmidi import XuMidiIn
            self.midi = XuMidiIn(0)
            if self.midi.ok:
                self.lb_midi.setText("MIDI: 已连接")
                self.lb_midi.setStyleSheet(f"color:{C_ORANGE};font-size:10px;")
            else:
                self.lb_midi.setText("MIDI: 无设备")
        except Exception as e:
            self.lb_midi.setText("MIDI: 不可用")
            self.midi = None

    # ------------------------------------------------------------------
    # 传输与参数
    # ------------------------------------------------------------------
    def on_play(self) -> None:
        self.engine.play()

    def on_stop(self) -> None:
        self.engine.stop()
        self.grid.set_playhead(-1)

    def _on_bpm(self, v: float) -> None:
        self.project.master.bpm = float(v)
        self.sl_bpm.blockSignals(True)
        self.sl_bpm.setValue(int(v))
        self.sl_bpm.blockSignals(False)
        self._mark_dirty()

    def _on_swing(self, v: int) -> None:
        self.project.master.swing = v / 100.0
        self.lb_swing.setText(f"{v}%")
        self._mark_dirty()

    def on_tap(self) -> None:
        """
        Tap Tempo：取最近 8 次点击，剔除异常值后求平均。

        为什么剔除最大最小：人手第一次点通常偏慢，偶尔会抖一下；
        去掉两个极端能明显让结果更稳。
        """
        now = time.perf_counter()
        if self._tap_times and now - self._tap_times[-1] > 2.0:
            self._tap_times.clear()  # 停顿超过 2 秒就重新开始
        self._tap_times.append(now)
        self._tap_times = self._tap_times[-8:]
        if len(self._tap_times) >= 2:
            iv = [self._tap_times[i] - self._tap_times[i - 1]
                  for i in range(1, len(self._tap_times))]
            iv = [x for x in iv if 0.15 <= x <= 2.0]  # 相当于 30..400 BPM
            if len(iv) >= 2:
                iv.remove(max(iv))
                iv.remove(min(iv))
            if iv:
                bpm = 60.0 / (sum(iv) / len(iv))
                bpm = max(40.0, min(300.0, bpm))
                self.sp_bpm.setValue(round(bpm, 1))

    def on_pattern(self, idx: int) -> None:
        self.engine.request_pattern(idx)
        self.sp_steps.blockSignals(True)
        self.sp_steps.setValue(self.pattern().steps)
        self.sp_steps.blockSignals(False)
        self.sp_len_changed()
        self.grid.refresh()

    def sp_len_changed(self) -> None:
        """pattern 切换后，把每轨长度控件同步成新 pattern 的值。"""
        for s in getattr(self, "strips", []):
            s.sp_len.blockSignals(True)
            s.sp_len.setValue(self.pattern().track_len[s.pad])
            s.sp_len.blockSignals(False)

    def on_steps(self, v: int) -> None:
        self.pattern().steps = int(v)
        self.grid.refresh()
        self._mark_dirty()

    def on_song_toggle(self) -> None:
        self.project.master.song_mode = self.cb_song.isChecked()
        self._mark_dirty()

    def on_song_edit(self) -> None:
        dlg = XuSongDialog(self.project, self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            dlg.apply()
            self._mark_dirty()

    def on_midi_clock(self, on: bool) -> None:
        self.engine.clock_external = bool(on)

    # ------------------------------------------------------------------
    # 网格交互
    # ------------------------------------------------------------------
    def on_hit_pad(self, pad: int) -> None:
        self.engine.trigger_pad(pad, 112, False)

    def on_toggle_step(self, pad: int, step: int) -> None:
        st = self.pattern().data[pad][step]
        st.on = not st.on
        if st.on:
            # 打开格子时顺手试听，符合硬件鼓机的操作直觉
            self.engine.trigger_pad(pad, st.velocity, st.accent)
        self.grid.update()
        self._mark_dirty()

    def on_edit_step(self, pad: int, step: int) -> None:
        st = self.pattern().data[pad][step]
        dlg = XuStepDialog(st, self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            dlg.apply()
            self.grid.update()
            self._mark_dirty()

    def on_toggle_mute(self, pad: int) -> None:
        p = self.project.pads[pad]
        p.mute = not p.mute
        self.strips[pad].bm.blockSignals(True)
        self.strips[pad].bm.setChecked(p.mute)
        self.strips[pad].bm.blockSignals(False)
        self.grid.update()

    def on_toggle_solo(self, pad: int) -> None:
        p = self.project.pads[pad]
        p.solo = not p.solo
        self.strips[pad].bs.blockSignals(True)
        self.strips[pad].bs.setChecked(p.solo)
        self.strips[pad].bs.blockSignals(False)
        self.grid.update()

    # ------------------------------------------------------------------
    # 主效果
    # ------------------------------------------------------------------
    def _on_lp(self, v: int) -> None:
        self.project.master.lp_cutoff = float(v)

    def _on_lpq(self, v: int) -> None:
        self.project.master.lp_q = v / 100.0

    def _on_hp(self, v: int) -> None:
        self.project.master.hp_cutoff = float(v)

    def _on_drive(self, v: int) -> None:
        self.project.master.drive = v / 100.0

    def _on_vol(self, v: int) -> None:
        self.project.master.volume = v / 100.0

    # ------------------------------------------------------------------
    # 定时刷新（UI 线程）
    # ------------------------------------------------------------------
    def on_tick(self) -> None:
        snap = self.engine.snapshot()
        step = int(snap["step"]) if snap["playing"] > 0.5 else -1
        self.grid.set_playhead(step)
        self.meter.set_levels(snap["meter_l"], snap["meter_r"])
        self.lb_stat.setText(f"CPU {snap['cpu'] * 100:4.1f}%  ·  XRUN {int(snap['xruns'])}")
        self.b_play.setStyleSheet(
            "QPushButton{font-weight:bold;font-size:13px;background:#5a2a08;border-color:%s;}"
            % C_ORANGE if snap["playing"] > 0.5 else "QPushButton{font-weight:bold;font-size:13px;}")
        # MIDI 事件轮询：解析放在 UI 线程，音频线程只负责发声
        if self.midi is not None and self.midi.ok:
            for status, d1, d2 in self.midi.poll():
                self._handle_midi(status, d1, d2)

    def _handle_midi(self, status: int, d1: int, d2: int) -> None:
        """处理 Note On / CC / Program Change / 时钟 / 开始停止。"""
        cmd = status & 0xF0
        if cmd == 0x90 and d2 > 0:
            # Note On：按映射查声部
            if d1 in PAD_MIDI_NOTES:
                pad = PAD_MIDI_NOTES.index(d1)
                self.engine.trigger_pad(pad, d2, d2 >= 120)
        elif cmd == 0xB0:
            # CC：7=音量，74=低通，71=共鸣
            if d1 == 7:
                self.sl_vol.setValue(int(d2 * 100 / 127))
            elif d1 == 74:
                # 对数映射更符合听感：20Hz..20kHz
                fc = 20.0 * (1000.0 ** (d2 / 127.0))
                self.sl_lp.setValue(int(max(200, min(20000, fc))))
            elif d1 == 71:
                self.sl_lpq.setValue(int(50 + d2 * 750 / 127))
        elif cmd == 0xC0:
            # Program Change：切 pattern（0..15）
            self.cb_pat.setCurrentIndex(min(d2, self.cb_pat.count() - 1))
        elif status == 0xFA:
            self.on_play()
        elif status == 0xFC:
            self.on_stop()
        elif status == 0xF8 and self.engine.clock_external:
            self.engine.external_clock_tick()

    # ------------------------------------------------------------------
    # 文件操作
    # ------------------------------------------------------------------
    def on_new(self) -> None:
        if not self._confirm_discard():
            return
        self.load_project(XuProject())

    def on_save(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "保存工程", "xu808.xu808",
                                              "XU-808 工程 (*.xu808 *.json)")
        if not path:
            return
        try:
            self.project.save(path)
            self._dirty = False
            self.statusBar().showMessage(f"已保存：{path}")
        except Exception as e:
            QMessageBox.critical(self, "保存失败", str(e))

    def on_load(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "打开工程", "",
                                              "XU-808 工程 (*.xu808 *.json)")
        if not path:
            return
        try:
            self.load_project(XuProject.load(path))
            self.statusBar().showMessage(f"已加载：{path}")
        except Exception as e:
            QMessageBox.critical(self, "加载失败", str(e))

    def load_project(self, proj: XuProject) -> None:
        """替换整个工程，并把所有控件拉回新值。"""
        self.engine.stop()
        self.project = proj
        self.engine.project = proj
        self.grid.project = proj
        self.cb_pat.blockSignals(True)
        self.cb_pat.clear()
        for i, p in enumerate(proj.patterns):
            self.cb_pat.addItem(f"{i + 1:02d} {p.name}")
        self.cb_pat.setCurrentIndex(proj.current_pattern)
        self.cb_pat.blockSignals(False)
        self.sp_bpm.blockSignals(True)
        self.sp_bpm.setValue(proj.master.bpm)
        self.sp_bpm.blockSignals(False)
        self.sl_bpm.blockSignals(True)
        self.sl_bpm.setValue(int(proj.master.bpm))
        self.sl_bpm.blockSignals(False)
        self.sl_swing.blockSignals(True)
        self.sl_swing.setValue(int(proj.master.swing * 100))
        self.sl_swing.blockSignals(False)
        self.lb_swing.setText(f"{int(proj.master.swing * 100)}%")
        self.cb_song.setChecked(proj.master.song_mode)
        self.sp_steps.blockSignals(True)
        self.sp_steps.setValue(proj.patterns[proj.current_pattern].steps)
        self.sp_steps.blockSignals(False)
        for s in self.strips:
            s.project = proj
            s.sync_from_project()
        self.grid.refresh()
        self._dirty = False

    def _confirm_discard(self) -> bool:
        if not self._dirty:
            return True
        r = QMessageBox.question(self, "未保存", "工程有未保存修改，继续会丢失。继续吗？")
        return r == QMessageBox.StandardButton.Yes

    def on_export_wav(self, mode: str) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "导出 WAV", f"xu808_{mode}.wav",
                                              "WAV 音频 (*.wav)")
        if not path:
            return
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            self.engine.export_wav(path, mode=mode, repeats=1)
            self.statusBar().showMessage(f"WAV 已导出：{path}")
        except Exception as e:
            QMessageBox.critical(self, "导出失败", str(e))
        finally:
            QApplication.restoreOverrideCursor()

    def on_export_stems(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "选择分轨输出目录")
        if not folder:
            return
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            files = self.engine.export_stems(folder)
            self.statusBar().showMessage(f"分轨已导出 {len(files)} 个文件到 {folder}")
        except Exception as e:
            QMessageBox.critical(self, "分轨导出失败", str(e))
        finally:
            QApplication.restoreOverrideCursor()

    def on_export_midi(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "导出 MIDI", "xu808.mid",
                                              "MIDI 文件 (*.mid)")
        if not path:
            return
        try:
            mode = "song" if self.project.master.song_mode else "pattern"
            export_midi(self.project, path, mode=mode, repeats=1)
            self.statusBar().showMessage(f"MIDI 已导出：{path}")
        except Exception as e:
            QMessageBox.critical(self, "导出失败", str(e))

    def on_about(self) -> None:
        QMessageBox.information(
            self, "关于 XU-808",
            "XU-808 十六声部模拟鼓机\n\n"
            "作者：徐某\n"
            "技术：PySide6 + PortAudio + NumPy/SciPy\n"
            "音序：48kHz 样本级时钟，16 步 / 64 步，支持 polymeter、\n"
            "swing、概率、微时、ratchet、重音、歌曲链。\n\n"
            "所有声音均为实时合成，不含任何采样。")

    # ------------------------------------------------------------------
    def keyPressEvent(self, ev) -> None:
        """键盘：空格播放/停止，数字键 1-8 切 pattern，Q/W/E… 敲鼓。"""
        if ev.key() == Qt.Key.Key_Space:
            self.engine.toggle()
            return
        if Qt.Key.Key_1 <= ev.key() <= Qt.Key.Key_8:
            idx = int(ev.key() - Qt.Key.Key_1)
            self.cb_pat.setCurrentIndex(idx)
            return
        # 16 键鼓垫映射（与硬件 808 的排布近似）
        keys = "ZXCVBNMASDFGHJK"
        ch = ev.text().upper()
        if ch and ch in keys:
            self.engine.trigger_pad(keys.index(ch), 110, False)
            return
        super().keyPressEvent(ev)

    def closeEvent(self, ev) -> None:
        """关闭顺序很重要：先停音频、再关 MIDI，最后才让窗口消失。"""
        try:
            self.engine.stop()
            self.engine.stop_audio()
            if self.midi is not None:
                self.midi.close()
        finally:
            super().closeEvent(ev)
