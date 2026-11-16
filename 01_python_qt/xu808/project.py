# -*- coding: utf-8 -*-
"""
Xu808 工程数据模型 —— Pattern、步进、音色、歌曲链、全局设置。

徐某把“数据”和“声音”彻底分家：这个文件里没有一行 DSP，
只有纯粹的数据结构与 JSON 序列化。这样做的原因有三：
  1. 保存/加载可以单独测试，不用开声卡；
  2. 任何语言版本都能照着这份结构实现互通（规格书第 7 节）；
  3. 音频线程只读不改，UI 线程改副本，天然避免数据竞争。
"""

from __future__ import annotations

import copy  # 深拷贝 pattern，避免“编辑时突然变声”
import json  # 工程文件标准格式，人类可读、易调试
from dataclasses import dataclass, field, asdict  # 数据类，省掉大量样板
from typing import Dict, List, Optional  # 类型提示

from .dsp import PAD_COUNT, XuPadParams  # 复用声部常量与参数类

# 工程文件格式标识与版本号；升级格式时必须递增 version
PROJECT_FORMAT = "XU808"
PROJECT_VERSION = 1

# 步进条件：规格书第 4 节。数值即存储值，不可随意改
COND_ALWAYS = 0  # 每循环都触发
COND_FILL = 1  # 只在 fill（填充）循环触发
COND_NOT_FILL = 2  # 只在非 fill 循环触发
COND_EVERY_2 = 3  # 每 2 个循环触发一次
COND_EVERY_4 = 4  # 每 4 个循环触发一次

COND_NAMES_CN = ["总是", "仅过门", "非过门", "每2循环", "每4循环"]


@dataclass
class XuStep:
    """
    一个格子（某轨某步）的全部属性。

    默认值必须与规格书一致：开=否、力度=100、概率=100、ratchet=1。
    徐某把每个属性都做成独立字段而不是位掩码，是为了序列化时一目了然，
    也方便其它语言直接用字典/结构体映射。
    """

    on: bool = False  # 是否启用
    velocity: int = 100  # 力度 1..127
    accent: bool = False  # 重音（同时改变音量/衰减/音色）
    prob: int = 100  # 触发概率 0..100（%）
    micro: int = 0  # 微时偏移，单位样本，负=提前
    ratchet: int = 1  # 滚奏次数 1..8
    cond: int = COND_ALWAYS  # 条件触发

    def key(self) -> str:
        """返回紧凑字符串，供 UI 在格子里显示 “A/75%/R3” 之类的信息。"""
        bits: List[str] = []
        if self.accent:
            bits.append("A")
        if self.prob < 100:
            bits.append(f"{self.prob}%")
        if self.ratchet > 1:
            bits.append(f"R{self.ratchet}")
        if self.micro != 0:
            bits.append(f"{self.micro:+d}")
        if self.cond != COND_ALWAYS:
            bits.append(COND_NAMES_CN[self.cond])
        return " ".join(bits)


@dataclass
class XuPattern:
    """
    一个 Pattern：16 条轨道 × 最多 64 步。

    为什么用“锯齿数组 + 固定 64 长度”：实时线程按索引直接访问，
    不需要在发声时判断长度，最坏情况也只是多几个空对象。
    """

    name: str = "PATTERN 1"
    steps: int = 16  # 本 pattern 的步数 1..64
    data: List[List[XuStep]] = field(default_factory=list)  # [16][64]
    track_len: List[int] = field(default_factory=list)  # 每轨独立长度（polymeter）
    fill: bool = False  # 是否“过门”循环（供条件触发使用）

    def __post_init__(self) -> None:
        """构造后补齐 16×64 的空格子，保证任何索引都安全。"""
        if not self.data:
            self.data = [[XuStep() for _ in range(64)] for _ in range(PAD_COUNT)]
        if not self.track_len:
            self.track_len = [16 for _ in range(PAD_COUNT)]

    def cell(self, pad: int, step: int) -> XuStep:
        """取格子；越界时返回一个临时空对象，绝不让实时线程抛异常。"""
        if 0 <= pad < PAD_COUNT and 0 <= step < 64:
            return self.data[pad][step]
        return XuStep()

    def clone(self) -> "XuPattern":
        """深拷贝：UI 编辑用副本，音频线程继续读旧对象，切换在步边界生效。"""
        return copy.deepcopy(self)

    def set_demo_beat(self) -> None:
        """
        写入一段默认 808 律动，让程序一打开就有声音。

        徐某选的是最经典的“boom-bap + 闭镲八分”：
        BD 0/8，SD 4/12，CH 每两格，OH 14，CP 12 附近加一点。
        这样用户第一下按空格就能听到完整节奏，而不是面对空白网格发呆。
        """
        for pad in range(PAD_COUNT):
            for s in range(64):
                self.data[pad][s] = XuStep()
        self.steps = 16
        # 底鼓：正拍 + 反拍，最后一格加个重音收尾
        for s in (0, 8):
            self.data[0][s].on = True
            self.data[0][s].velocity = 118 if s == 0 else 105
            self.data[0][s].accent = s == 0
        self.data[0][14].on = True
        self.data[0][14].velocity = 90
        # 军鼓：2、4 拍
        for s in (4, 12):
            self.data[1][s].on = True
            self.data[1][s].velocity = 110
            self.data[1][s].accent = s == 12
        # 闭镲：每个八分音符，力度有呼吸感
        for s in range(0, 16, 2):
            self.data[15][s].on = True
            self.data[15][s].velocity = 84 if s % 4 else 96
        # 开镲：第 14 步
        self.data[14][14].on = True
        self.data[14][14].velocity = 92
        # 拍手：第 12 步叠在军鼓上
        self.data[10][12].on = True
        self.data[10][12].velocity = 80
        # 沙锤：奇数格轻点，增加律动
        for s in range(1, 16, 2):
            self.data[11][s].on = True
            self.data[11][s].velocity = 42
        # 演示 ratchet：第 15 步底鼓滚 3 下（规格书要求的功能，直接可听）
        self.data[0][15].on = True
        self.data[0][15].velocity = 88
        self.data[0][15].ratchet = 3
        self.name = "PATTERN 1"


@dataclass
class XuMaster:
    """主输出效果与全局设置。数值范围见规格书第 6 节。"""

    volume: float = 0.80  # 主音量 0..1
    drive: float = 1.35  # 过载 1..8
    lp_cutoff: float = 18000.0  # 低通截止 Hz
    lp_q: float = 0.90  # 低通 Q
    hp_cutoff: float = 28.0  # 高通截止 Hz（去掉直流/次声）
    hp_q: float = 0.70
    lp_on: bool = True
    hp_on: bool = True
    bpm: float = 120.0
    swing: float = 0.0  # 0..0.75
    beats_per_bar: int = 4  # 拍号分子
    beat_unit: int = 4  # 拍号分母（4=四分音符，8=八分音符）
    song_mode: bool = False


@dataclass
class XuSongEntry:
    """歌曲链的一节：播放哪个 pattern、重复几遍。"""

    pattern: int = 0
    repeats: int = 1


@dataclass
class XuSong:
    """歌曲链；enabled=False 时只循环当前 pattern。"""

    enabled: bool = False
    entries: List[XuSongEntry] = field(default_factory=list)


@dataclass
class XuProject:
    """
    顶层工程：包含 64 个 pattern、16 组音色、主效果、歌曲链。

    徐某把 pattern 数量定为 64：既够写一整首歌，又不会让 JSON 大到离谱。
    """

    name: str = "徐某的 808 工程"
    patterns: List[XuPattern] = field(default_factory=list)
    pads: List[XuPadParams] = field(default_factory=list)
    master: XuMaster = field(default_factory=XuMaster)
    song: XuSong = field(default_factory=XuSong)
    current_pattern: int = 0
    seed: int = 0x808808  # 概率触发的确定性随机种子

    def __post_init__(self) -> None:
        """补齐默认数据：16 个 pattern + 16 组音色参数。"""
        if not self.patterns:
            for i in range(16):
                pat = XuPattern(name=f"PATTERN {i + 1}")
                if i == 0:
                    pat.set_demo_beat()
                self.patterns.append(pat)
        while len(self.patterns) < 16:
            self.patterns.append(XuPattern(name=f"PATTERN {len(self.patterns) + 1}"))
        if not self.pads:
            self.pads = [XuPadParams() for _ in range(PAD_COUNT)]
        while len(self.pads) < PAD_COUNT:
            self.pads.append(XuPadParams())

    # ------------------------------------------------------------------
    # 序列化
    # ------------------------------------------------------------------
    def to_dict(self) -> Dict:
        """
        转成可 JSON 化的字典。

        知识点：dataclasses.asdict 会递归转换，但 pattern 里的 1024 个
        XuStep 会让文件很大（约 200KB），这仍然可接受；徐某优先保证
        “任何一个字段都不丢”，而不是文件大小。
        """
        return {
            "format": PROJECT_FORMAT,
            "version": PROJECT_VERSION,
            "name": self.name,
            "seed": self.seed,
            "current_pattern": self.current_pattern,
            "master": asdict(self.master),
            "song": {
                "enabled": self.song.enabled,
                "entries": [asdict(e) for e in self.song.entries],
            },
            "pads": [asdict(p) for p in self.pads],
            "patterns": [
                {
                    "name": p.name,
                    "steps": p.steps,
                    "fill": p.fill,
                    "track_len": list(p.track_len),
                    "data": [[asdict(c) for c in row] for row in p.data],
                }
                for p in self.patterns
            ],
        }

    def save(self, path: str) -> None:
        """写工程文件。用 UTF-8 + ensure_ascii=False，中文名不会变成乱码。"""
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, ensure_ascii=False, indent=1)

    @staticmethod
    def from_dict(d: Dict) -> "XuProject":
        """
        从字典恢复工程，并做“缺字段用默认值”的容错。

        为什么要容错：规格书第 7 节承诺旧工程能在新版本打开。
        徐某在这里逐字段 get，而不是直接下标，就是为了这个承诺。
        """
        proj = XuProject(name=str(d.get("name", "徐某的 808 工程")))
        proj.seed = int(d.get("seed", 0x808808))
        proj.current_pattern = int(d.get("current_pattern", 0))
        # ---- 主设置 ----
        m = d.get("master", {})
        for key, val in asdict(proj.master).items():
            if key in m:
                try:
                    setattr(proj.master, key, type(val)(m[key]))
                except (TypeError, ValueError):
                    pass  # 坏数据就保留默认值，绝不因为一个字段打不开工程
        # ---- 音色 ----
        pads = d.get("pads", [])
        for i in range(min(PAD_COUNT, len(pads))):
            src = pads[i] or {}
            for key, val in asdict(proj.pads[i]).items():
                if key in src:
                    try:
                        setattr(proj.pads[i], key, type(val)(src[key]))
                    except (TypeError, ValueError):
                        pass
        # ---- Pattern ----
        pats = d.get("patterns", [])
        proj.patterns = []
        for i, src in enumerate(pats):
            pat = XuPattern(name=str(src.get("name", f"PATTERN {i + 1}")))
            pat.steps = max(1, min(64, int(src.get("steps", 16))))
            pat.fill = bool(src.get("fill", False))
            tl = src.get("track_len", [])
            for t in range(min(PAD_COUNT, len(tl))):
                pat.track_len[t] = max(1, min(64, int(tl[t])))
            rows = src.get("data", [])
            for pad in range(min(PAD_COUNT, len(rows))):
                cols = rows[pad] or []
                for s in range(min(64, len(cols))):
                    c = cols[s] or {}
                    st = pat.data[pad][s]
                    st.on = bool(c.get("on", False))
                    st.velocity = max(1, min(127, int(c.get("velocity", 100))))
                    st.accent = bool(c.get("accent", False))
                    st.prob = max(0, min(100, int(c.get("prob", 100))))
                    st.micro = max(-2400, min(2400, int(c.get("micro", 0))))
                    st.ratchet = max(1, min(8, int(c.get("ratchet", 1))))
                    st.cond = max(0, min(4, int(c.get("cond", 0))))
            proj.patterns.append(pat)
        if not proj.patterns:
            proj.patterns = [XuPattern()]
        # ---- 歌曲链 ----
        song = d.get("song", {})
        proj.song.enabled = bool(song.get("enabled", False))
        proj.song.entries = []
        for e in song.get("entries", []):
            proj.song.entries.append(
                XuSongEntry(pattern=max(0, int(e.get("pattern", 0))),
                            repeats=max(1, int(e.get("repeats", 1)))))
        # 保证 current_pattern 合法，否则播放时会越界
        proj.current_pattern = max(0, min(len(proj.patterns) - 1, proj.current_pattern))
        return proj

    @staticmethod
    def load(path: str) -> "XuProject":
        """读工程文件；任何 IO/JSON 错误都交给调用方用对话框提示。"""
        with open(path, "r", encoding="utf-8") as f:
            return XuProject.from_dict(json.load(f))
