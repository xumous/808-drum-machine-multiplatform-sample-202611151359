# -*- coding: utf-8 -*-
"""
Xu808 DSP 核心 —— 纯算法模拟合成，绝不使用任何预录制采样。

徐某在 2026 年深秋的夜里写下这个文件时，给自己定了一条死规矩：
每一个声音都必须由“振荡器 + 噪声 + 滤波器 + 包络”当场算出来，
因为只有这样才能像真正的 TR-808 那样，让 Tune/Decay/Tone 旋钮
真的改变声音的“骨骼”，而不是在几条固定采样之间做淡入淡出。

本模块只依赖 numpy 与 scipy.signal（后者只用来跑双二阶滤波器的
差分方程，C 语言实现，速度足够）。它被 engine.py 在音频回调线程里
调用，因此这里所有函数都遵守三条纪律：
  1. 不弹窗、不打印、不读写文件；
  2. 不创建 Qt 对象；
  3. 每块只做定量的 numpy 向量运算，绝不按样本写 Python 循环。
"""

from __future__ import annotations  # 让类型注解在运行时不必求值，略微提速

import math  # 数学常数与 exp/sin 等标量函数
from dataclasses import dataclass  # 用数据类描述“每声部参数”，可读性高
from typing import List  # 仅用于类型提示

import numpy as np  # 向量化 DSP 的绝对主力
from scipy.signal import lfilter  # 双二阶 IIR 的 C 实现，带状态 zi，适合实时

# ---------------------------------------------------------------------------
# 全局常量：所有语言版本必须与此一致，否则各版本听感会对不上
# ---------------------------------------------------------------------------
SAMPLE_RATE = 48000  # 采样率 48kHz，徐某选的“专业音频入门标配”
PAD_COUNT = 16  # 声部数量，规格书第 2 节规定

# 声部代号，顺序即编号，任何移植都不得改变
PAD_CODES = ["BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC",
             "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"]

# 中文名，用于 UI 显示；徐某坚持界面也要让初学者一眼看懂
PAD_NAMES_CN = ["底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加",
                "边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲"]

# 通用 MIDI 打击乐音高映射（规格书第 7 节），导出 MIDI 时使用
PAD_MIDI_NOTES = [36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42]

# 每个声部最多同时存在的 voice 数；超过就抢占最老的那个
MAX_VOICES_PER_PAD = 4


def _db_to_lin(db: float) -> float:
    """分贝转线性增益。知识：20*log10(x)=dB，所以 x=10^(dB/20)。不难。"""
    return float(10.0 ** (db / 20.0))


class XuBiquad:
    """
    RBJ Audio EQ Cookbook 双二阶滤波器（Direct Form II Transposed）。

    为什么自己写系数：规格书要求截止频率/共鸣实时可调，且切换时不能爆音。
    RBJ 公式是音频界的“九九乘法表”，任何语言移植都能照抄，误差可控。

    状态 zi 必须跨块保留——这是“滤波器记忆”的物理体现；
    如果每块清零，声音每 10ms 就会“咔”一下，徐某在早期版本里踩过这个坑。
    """

    __slots__ = ("b0", "b1", "b2", "a1", "a2", "zi", "_sr", "_kind", "_fc", "_q")

    def __init__(self, sr: int = SAMPLE_RATE) -> None:
        # 初始化为“直通”：b=[1,0,0]，a=[1,0,0]，输出等于输入
        self.b0, self.b1, self.b2 = 1.0, 0.0, 0.0
        self.a1, self.a2 = 0.0, 0.0
        # zi 长度固定为 2，形状 (2,)，dtype float64 足够；float32 在低频也够
        self.zi = np.zeros(2, dtype=np.float64)
        self._sr = sr
        self._kind = "bypass"
        self._fc = 0.0
        self._q = 0.707

    # -- 以下三个 set_* 只在参数变化时调用，不做逐样本运算 -------------------
    def set_lowpass(self, fc: float, q: float = 0.707) -> None:
        """设置低通：保留低频，砍掉高频。fc 单位 Hz。"""
        sr = self._sr
        # 截止频率必须落在 (0, Nyquist) 开区间，否则 tan() 会炸成负数/无穷
        fc = min(max(fc, 20.0), sr * 0.49)
        q = min(max(q, 0.1), 20.0)
        # 系数没变就直接返回：每块都重算三角函数是纯浪费，实时线程里能省则省
        if self._kind == "lp" and abs(fc - self._fc) < 0.5 and abs(q - self._q) < 1e-3:
            return
        w0 = 2.0 * math.pi * fc / sr  # 数字角频率
        cw = math.cos(w0)  # 余弦，公式里反复用
        alpha = math.sin(w0) / (2.0 * q)  # 品质因数决定共振峰宽度
        a0 = 1.0 + alpha
        # RBJ 低通系数，全部除以 a0 归一化，便于直接差分
        self.b0 = (1.0 - cw) * 0.5 / a0
        self.b1 = (1.0 - cw) / a0
        self.b2 = self.b0
        self.a1 = (-2.0 * cw) / a0
        self.a2 = (1.0 - alpha) / a0
        self._kind, self._fc, self._q = "lp", fc, q

    def set_highpass(self, fc: float, q: float = 0.707) -> None:
        """设置高通：砍掉低频，保留高频。做“电话音”或去掉直流都用它。"""
        sr = self._sr
        fc = min(max(fc, 10.0), sr * 0.49)
        q = min(max(q, 0.1), 20.0)
        if self._kind == "hp" and abs(fc - self._fc) < 0.5 and abs(q - self._q) < 1e-3:
            return
        w0 = 2.0 * math.pi * fc / sr
        cw = math.cos(w0)
        alpha = math.sin(w0) / (2.0 * q)
        a0 = 1.0 + alpha
        self.b0 = (1.0 + cw) * 0.5 / a0
        self.b1 = -(1.0 + cw) / a0
        self.b2 = self.b0
        self.a1 = (-2.0 * cw) / a0
        self.a2 = (1.0 - alpha) / a0
        self._kind, self._fc, self._q = "hp", fc, q

    def set_bandpass(self, fc: float, q: float = 1.2) -> None:
        """常量峰值增益带通（0 dB 峰值），做军鼓噪声、牛铃、金属簇都用它。"""
        sr = self._sr
        fc = min(max(fc, 20.0), sr * 0.49)
        q = min(max(q, 0.1), 30.0)
        if self._kind == "bp" and abs(fc - self._fc) < 0.5 and abs(q - self._q) < 1e-3:
            return
        w0 = 2.0 * math.pi * fc / sr
        cw = math.cos(w0)
        alpha = math.sin(w0) / (2.0 * q)
        a0 = 1.0 + alpha
        self.b0 = alpha / a0
        self.b1 = 0.0
        self.b2 = -alpha / a0
        self.a1 = (-2.0 * cw) / a0
        self.a2 = (1.0 - alpha) / a0
        self._kind, self._fc, self._q = "bp", fc, q

    def process(self, x: np.ndarray) -> np.ndarray:
        """
        对一整块信号做滤波。

        知识点：scipy.signal.lfilter 的差分方程是
            y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
        zi 保存过去两个输入/输出状态；返回值里我们只取 y，把 zf 写回 self.zi，
        下一块才能无缝接上。这就是“实时滤波”的全部秘密。
        """
        if x.size == 0:
            return x
        # a[0] 固定为 1，因为 set_* 已经归一化过
        b = (self.b0, self.b1, self.b2)
        a = (1.0, self.a1, self.a2)
        # lfilter 要求 float64 才能稳定处理 30Hz 这种低音，故转换后算，返回前再转 float32
        y, self.zi = lfilter(b, a, x, zi=self.zi)
        return y


@dataclass
class XuPadParams:
    """
    单个声部（音轨）的用户参数。

    注意：这些值是“用户意图”，不是 DSP 立即使用的物理量；
    engine 会把它们传进来，再由 Voice 在每个块换算成频率/时间常数。
    这样 UI 可以随便拖旋钮，声音线程只做无分配的读取。
    """

    tune: float = 0.50  # 音高，0..1 映射到各声部自己的频率范围
    decay: float = 0.50  # 衰减，0..1 映射到各声部自己的时间范围
    tone: float = 0.50  # 音色（滤波中心/点击量）
    snappy: float = 0.55  # 军鼓专用：响弦噪声量
    tail: float = 0.45  # 拍手专用：尾巴噪声量
    level: float = 0.75  # 推子 0..1.25
    pan: float = 0.50  # 声像 0=左 1=右
    mute: bool = False  # 静音（在混音阶段阻断，不是停止触发）
    solo: bool = False  # 独奏


class XuDrumVoice:
    """
    一个“鼓槌”。每次敲击创建一个 voice 实例，声音自然衰减完就回收。

    徐某特意没有做“一次性生成整段波形再播放”，因为那样 Decay 旋钮
    在声音进行中就不能实时改变，而且 16 声部叠加会疯狂占内存。
    这里采用“按块解析式求值”：声音的相位、包络都是时间的闭式函数，
    每块只计算属于本块的那一小段，因此既实时又省 CPU，还天然没有循环残留
    ——上一槌的时间到了就自动归零，绝不会越叠越多。
    """

    # 注意：accent 必须列在 __slots__ 里，否则 trigger() 里赋值会抛 AttributeError。
    # 徐某第一次写的时候就漏了它，程序在第一次敲鼓时直接崩掉，教训深刻。
    __slots__ = ("pad", "active", "start", "stop", "noise", "npos",
                 "bp", "hp", "bp2", "hp2", "gain", "seed", "accent", "fade_len")

    def __init__(self, pad: int, seed: int = 12345) -> None:
        self.pad = pad  # 属于哪个声部（0..15）
        self.active = False  # 是否正在发声
        self.start = 0  # 触发时刻（绝对样本号）
        self.stop = 1 << 62  # 强制停止时刻（choke 用；默认永不）
        # 预生成 8192 个白噪声样本，避免在回调里调用随机数生成器；
        # 8192 个 float32 = 32KB，16 声部×4 voice 也就 2MB，完全可接受
        self.noise = np.random.default_rng(seed + pad * 7919).standard_normal(8192).astype(np.float32)
        self.npos = 0  # 噪声读取指针，循环使用
        self.bp = XuBiquad()  # 主带通（军鼓/牛铃/拍手/镲）
        self.hp = XuBiquad()  # 主高通（沙锤/镲）
        self.bp2 = XuBiquad()  # 备用滤波器（需要两级时使用）
        self.hp2 = XuBiquad()
        self.gain = 1.0  # 触发时的力度增益（含重音）
        self.seed = seed
        self.accent = False  # 本次触发是否重音（影响音色，不只是音量）
        self.fade_len = 64  # 被掐断时的淡出长度（样本），防止“咔哒”爆音

    # ------------------------------------------------------------------
    # 触发 / 停止
    # ------------------------------------------------------------------
    def trigger(self, when: int, gain: float, accent: bool) -> None:
        """
        在绝对样本时刻 when 重新触发。

        重音 accent 会同时改变音量、包络和音色（规格书第 4 节硬性要求），
        因此这里把 accent 存进 gain 并在 render 里再用它调整衰减与滤波。
        """
        self.active = True
        self.start = int(when)
        self.stop = 1 << 62  # 清除上一次的 choke，允许再次发声
        self.gain = float(gain)
        self.npos = 0
        # 重置滤波器状态，避免上一槌的残余能量造成“噗”声
        # 注意：这里做的是赋值而非分配，zi 数组本身已经存在
        self.bp.zi[:] = 0.0
        self.hp.zi[:] = 0.0
        self.bp2.zi[:] = 0.0
        self.hp2.zi[:] = 0.0
        # 重音标志单独保存：808 的重音会让包络更长、滤波更亮，
        # 如果只乘一个 1.3 的增益，听感会“假”，所以必须把布尔值带进合成器。
        self.accent = accent

    def kill(self, when: int, fade: int = 24) -> None:
        """
        在 when 时刻强行掐断（闭镲掐开镲、voice stealing 都靠它）。

        注意：真正的样本输出会多做 fade 个样本的线性淡出。
        为什么：波形在非零处被硬切会产生宽带“咔哒”声，
        24 个样本（0.5ms）就足以消除它，又不会让人觉得声音拖沓。
        """
        if self.active:
            # 关键：stop 要设在 when+fade，而不是 when。
            # 因为淡出斜坡发生在 stop 之前的 fade 个样本里；如果 stop=when，
            # 未来样本全在 stop 之后，就变成硬切，仍会“咔哒”。
            self.stop = min(self.stop, int(when) + int(fade))
            self.fade_len = max(1, int(fade))

    def _take_noise(self, n: int) -> np.ndarray:
        """
        从循环噪声池里取 n 个样本，返回的是“视图”而非新数组（零拷贝）。

        如果剩余不够就回到 0 重新开始：噪声本身无周期性可言，
        这一点点不连续在鼓声里完全听不出来，却换来了回调内零分配。
        """
        if n >= self.noise.size:
            n = self.noise.size
        if self.npos + n > self.noise.size:
            self.npos = 0
        seg = self.noise[self.npos:self.npos + n]
        self.npos += n
        return seg

    # ------------------------------------------------------------------
    # 核心：渲染一块（绝对样本区间 [b0, b1)）
    # ------------------------------------------------------------------
    def render(self, buf: np.ndarray, b0: int, b1: int, p: XuPadParams) -> None:
        """
        把本 voice 在 [b0,b1) 内的声音叠加进 buf。

        参数 buf 形如 (m, 2) 的 float32 立体声片段，本函数只负责往里面 += 。
        p 是当前声部参数快照（engine 传入的是深拷贝，UI 线程改不到它）。
        """
        if not self.active:
            return
        a = max(b0, self.start)  # 实际开始样本
        b = min(b1, self.stop)  # 实际结束样本
        if b <= a:
            # 本块完全在触发前或已被掐断：什么都不做，但若已过停止点就回收
            if b1 > self.stop:
                self.active = False
            return
        m = b - a
        if m <= 0:
            return
        # 相对触发点的“声音年龄”（秒），这是所有解析包络的自变量
        t0 = (a - self.start) / SAMPLE_RATE
        t = t0 + np.arange(m, dtype=np.float64) / SAMPLE_RATE

        # 每个声部一个独立的合成函数；返回单声道 float32 片段
        sig = self._synth(t, m, p)

        # -------- 掐断淡出 --------
        # 如果本 voice 已被 kill/release，则最后 fade_len 个样本线性淡出，
        # 既保证“说停就停”，又不产生点击声。
        if self.stop < (1 << 60):
            idx = a + np.arange(m, dtype=np.int64)
            dist = (self.stop - idx).astype(np.float64)
            ramp = np.clip(dist / float(max(1, self.fade_len)), 0.0, 1.0)
            sig = (sig * ramp).astype(np.float32)

        # -------- 力度/重音增益 + 等功率声像 --------
        # 等功率声像：pan=0.5 时左右各 0.707，总功率恒定，扫声像不会忽大忽小
        pan = min(max(p.pan, 0.0), 1.0)
        lg = math.cos(pan * math.pi * 0.5)  # 左增益
        rg = math.sin(pan * math.pi * 0.5)  # 右增益
        # level 也在这里乘，mute/solo 由 engine 在混音前判断（见 engine.render_block）
        g = self.gain * max(p.level, 0.0) * 0.42  # 0.42 是经验性总线余量，防 16 声部叠加削波
        # 用一个切片原地累加，避免创建临时大数组
        buf[a - b0:b - b0, 0] += sig * (g * lg)
        buf[a - b0:b - b0, 1] += sig * (g * rg)

        # 自然衰减结束（超过最大寿命）就回收，保证“上一个音过后一定要停止”
        if b1 - self.start > int(4.0 * SAMPLE_RATE):
            self.active = False

    # ------------------------------------------------------------------
    def _synth(self, t: np.ndarray, m: int, p: XuPadParams) -> np.ndarray:
        """
        根据声部编号分派到具体合成算法。

        徐某把 16 个声部分成五族：
          低频正弦族（BD/嗵鼓/康加）、双振荡器族（SD）、
          噪声族（RS/CP/MA/CL 的一部分）、金属族（CB/CY/OH/CH）。
        每族共享数学骨架，但参数映射不同——这就是 808 音色的“家族相似性”。
        """
        pad = self.pad

        # ================= 0 底鼓 BD =================
        if pad == 0:
            # 基频 40..80Hz；Tone 越高起始扫频越夸张
            f_end = 40.0 + p.tune * 40.0
            f_start = f_end * (1.6 + p.tone * 1.9)
            tau = 0.018 + (1.0 - p.decay) * 0.012  # 音高下滑时间常数
            # 频率的积分：∫(f_end + Δf·e^{-t/τ})dt = f_end·t + Δf·τ(1-e^{-t/τ})
            d_f = f_start - f_end
            phase = 2.0 * math.pi * (f_end * t + d_f * tau * (1.0 - np.exp(-t / tau)))
            # 幅度包络：Decay 0..1 → 0.12..1.6 秒；重音再延长 20%
            dec = (0.12 + p.decay * 1.48) * (1.18 if self.accent else 1.0)
            amp = np.exp(-t / dec)
            # 起音“点击”：一段极短高通噪声，Tone 控制其比例
            n = self._take_noise(m).astype(np.float64)
            self.hp.set_highpass(1800.0 + p.tone * 3000.0, 0.8)
            click = self.hp.process(n) * np.exp(-t / 0.0035) * (0.06 + p.tone * 0.32)
            # 808 底鼓的“桥接 T 振荡器”失真感：对正弦做轻微波形整形
            body = np.sin(phase)
            body = np.tanh(body * (1.15 + p.decay * 1.2)) * 0.92
            return (body * amp + click).astype(np.float32)

        # ================= 1 军鼓 SD =================
        if pad == 1:
            f1 = 150.0 + p.tune * 110.0  # 鼓皮基频
            f2 = f1 * (1.78 + p.tone * 0.22)  # 第二膜，略有失谐才有“响弦感”
            # 两个正弦各自指数衰减，第二膜略快
            dec_o = 0.055 + p.decay * 0.14
            osc = (0.55 * np.sin(2.0 * math.pi * f1 * t) * np.exp(-t / dec_o)
                   + 0.34 * np.sin(2.0 * math.pi * f2 * t) * np.exp(-t / (dec_o * 0.72)))
            # 响弦噪声：带通中心随 Snappy 升高，噪声比也随 Snappy 增大
            n = self._take_noise(m).astype(np.float64)
            self.bp.set_bandpass(1200.0 + p.snappy * 3800.0, 0.9 + p.tone * 0.8)
            nz = self.bp.process(n)
            dec_n = (0.075 + p.decay * 0.16) * (0.85 if self.accent else 1.0)
            nz = nz * np.exp(-t / dec_n) * (0.25 + p.snappy * 0.7)
            return (osc * 0.7 + nz).astype(np.float32)

        # ================= 2..7 嗵鼓 / 康加 =================
        if 2 <= pad <= 7:
            # 嗵鼓基频：80/120/170；康加：220/300/400；Tune 统一乘 0.6..1.6
            if pad <= 4:
                base = (80.0, 120.0, 170.0)[pad - 2]
                decay_s = 0.15 + p.decay * 0.75
                click_amt = 0.04 + p.tone * 0.22
                sweep = 1.9
            else:
                base = (220.0, 300.0, 400.0)[pad - 5]
                decay_s = 0.10 + p.decay * 0.55
                click_amt = 0.02 + p.tone * 0.12
                sweep = 1.35
            f0 = base * (0.6 + p.tune * 1.0)
            tau = 0.035 if pad <= 4 else 0.02
            # 起始频率高、迅速落到基频，这是“鼓皮张力释放”的听感来源
            phase = 2.0 * math.pi * (f0 * t + f0 * (sweep - 1.0) * tau * (1.0 - np.exp(-t / tau)))
            amp = np.exp(-t / decay_s)
            n = self._take_noise(m).astype(np.float64)
            self.hp.set_highpass(2500.0, 0.7)
            click = self.hp.process(n) * np.exp(-t / 0.004) * click_amt
            return (np.sin(phase) * amp * 0.9 + click).astype(np.float32)

        # ================= 8 边击 RS =================
        if pad == 8:
            f = 1200.0 + p.tune * 1400.0
            dec = 0.012 + p.decay * 0.05
            # 方波穿过带通 → 木质的“哒”声；再加一点噪声模拟鼓棒接触
            sq = np.sign(np.sin(2.0 * math.pi * f * t))
            self.bp.set_bandpass(f, 2.2)
            body = self.bp.process(sq) * np.exp(-t / dec)
            n = self._take_noise(m).astype(np.float64)
            self.hp.set_highpass(3000.0, 0.8)
            tick = self.hp.process(n) * np.exp(-t / 0.004) * 0.35
            return (body * 0.8 + tick).astype(np.float32)

        # ================= 9 击木 CL =================
        if pad == 9:
            f = 2000.0 + p.tune * 1000.0
            dec = 0.018 + p.decay * 0.035
            return (np.sin(2.0 * math.pi * f * t) * np.exp(-t / dec) * 0.9).astype(np.float32)

        # ================= 10 拍手 CP =================
        if pad == 10:
            # 经典 808 拍手：4 次极快的噪声爆发 + 一段较长的混响尾巴
            n = self._take_noise(m).astype(np.float64)
            self.bp.set_bandpass(800.0 + p.tone * 1700.0, 1.1)
            nz = self.bp.process(n)
            env = np.zeros_like(t)
            # 四次爆发间隔约 9ms，徐某把它写成加法而不是串联，便于向量化
            for k in range(4):
                tk = k * 0.009
                # 只让 t>=tk 的部分参与，用 np.where 实现“延迟门”
                env += np.where(t >= tk, np.exp(-(t - tk) / 0.011), 0.0) * 0.55
            # 尾巴：Decay 控制长度，Tail 控制比例
            tail = np.exp(-t / (0.05 + p.decay * 0.35)) * (0.15 + p.tail * 0.55)
            return (nz * (env + tail)).astype(np.float32)

        # ================= 11 沙锤 MA =================
        if pad == 11:
            n = self._take_noise(m).astype(np.float64)
            self.hp.set_highpass(4000.0 + p.tone * 5000.0, 0.9)
            nz = self.hp.process(n)
            # 极快的起音 + 短衰减，模拟沙粒撞击
            env = (1.0 - np.exp(-t / 0.0015)) * np.exp(-t / (0.018 + p.decay * 0.12))
            return (nz * env * 0.85).astype(np.float32)

        # ================= 12 牛铃 CB =================
        if pad == 12:
            f = 400.0 + p.tune * 300.0
            # 两个方波（频率比 1:1.48）是 808 牛铃的招牌
            sq1 = np.sign(np.sin(2.0 * math.pi * f * t))
            sq2 = np.sign(np.sin(2.0 * math.pi * f * 1.48 * t))
            self.bp.set_bandpass(2400.0 + p.tone * 1200.0, 1.4)
            body = self.bp.process(sq1 * 0.5 + sq2 * 0.5)
            dec = 0.05 + p.decay * 0.35
            return (body * np.exp(-t / dec) * 0.85).astype(np.float32)

        # ================= 13..15 吊镲 / 开镲 / 闭镲 =================
        # 808 的镲不是真镲片采样，而是 6 个方波按“非谐和比例”叠加，
        # 再经高通+带通得到金属味。这个比例来自对 808 电路的逆向分析。
        ratios = (1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523)
        root = 320.0 * (0.75 + p.tune * 0.5)
        metal = np.zeros(m, dtype=np.float64)
        for r in ratios:
            metal += np.sign(np.sin(2.0 * math.pi * root * r * t))
        metal *= 1.0 / len(ratios)
        # 高通切掉低频“嗡嗡”，带通突出金属频段
        self.hp.set_highpass(5200.0 + p.tone * 2600.0, 0.8)
        metal = self.hp.process(metal)
        self.bp.set_bandpass(7600.0 + p.tone * 3600.0, 0.9)
        metal = self.bp.process(metal)
        if pad == 15:  # 闭镲：极短
            dec = 0.028 + p.decay * 0.20
        elif pad == 14:  # 开镲：长
            dec = 0.12 + p.decay * 0.65
        else:  # 吊镲：最长，并带一点噪声尾巴
            dec = 0.25 + p.decay * 1.75
        env = np.exp(-t / dec)
        out = metal * env
        if pad == 13:
            # 吊镲额外混一点高频噪声，增加“叮”的空气感
            n = self._take_noise(m).astype(np.float64)
            self.hp2.set_highpass(9000.0, 0.7)
            out = out + self.hp2.process(n) * env * 0.12
        return (out * 0.75).astype(np.float32)
