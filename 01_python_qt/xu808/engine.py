# -*- coding: utf-8 -*-
"""
Xu808 实时音频引擎 —— 唯一权威时钟就在这个文件里。

徐某写这个文件时反复念叨一句话：
“绝不能让 QTimer 或 time.sleep 去决定鼓点。”
所以这里维护了一个 int64 的绝对样本计数器 sample_pos，
每一次回调都先把本块内应该发生的步进事件算出来，再按样本偏移切块渲染。
这样即使系统调度抖动 20ms，鼓点仍然精确落在它该落的采样上。

线程模型：
  GUI 线程  ──写──>  project（普通 Python 对象，属性赋值是原子的）
  音频线程  ──读──>  project + 自己的 voice 池 + 事件队列
  音频线程  ──写──>  snapshot（int/float 字典）
  GUI 线程  ──读──>  snapshot（只读，不阻塞）
PortAudio 的回调线程是 C 线程，Python 代码在其中持有 GIL 运行；
因此回调里绝不做文件 IO、弹窗、打印或长时间运算。
"""

from __future__ import annotations

import copy  # 离线渲染使用工程副本，避免干扰现场
import math  # 标量数学
import struct  # 写 MIDI 变长量时用到
import time  # 仅用于统计 CPU 占用，不在回调里 sleep
import wave  # 标准库 WAV 写入，零依赖
from typing import Dict, List, Optional, Tuple

import numpy as np  # 音频块运算

from .dsp import (MAX_VOICES_PER_PAD, PAD_COUNT, PAD_MIDI_NOTES, SAMPLE_RATE,
                  XuDrumVoice, XuPadParams)
from .project import (COND_ALWAYS, COND_EVERY_2, COND_EVERY_4, COND_FILL,
                      COND_NOT_FILL, XuPattern, XuProject)

# 实时块大小：512 样本 @48k = 10.67ms，是延迟与稳定性的折中
BLOCK_SIZE = 512

# 事件元组定义：(绝对样本, 声部编号, 力度0..1, 是否重音)
XuEvent = Tuple[int, int, float, bool]


class XuEngine:
    """
    引擎本体：音序器 + 混音 + 主效果 + 设备输出 + 离线导出。

    一个 XuEngine 同时服务“实时播放”和“离线渲染”，因为规格书要求
    导出必须复用同一套 DSP，绝不允许“实时好听、导出变样”。
    """

    def __init__(self, project: XuProject, sr: int = SAMPLE_RATE) -> None:
        self.project = project  # 当前工程（引用，不复制）
        self.sr = sr  # 采样率
        # ---- voice 池：16 声部 × 4 并发，构造时一次性分配好 ----
        self.voices: List[List[XuDrumVoice]] = [
            [XuDrumVoice(pad, seed=0x808 + pad) for _ in range(MAX_VOICES_PER_PAD)]
            for pad in range(PAD_COUNT)
        ]
        # ---- 传输状态 ----
        self.playing = False  # 是否正在走带
        self.sample_pos = 0  # 下一块的绝对起始样本（int64 语义）
        self.next_step_abs = 0  # 下一个 16 分步的绝对样本
        self.step_index = 0  # 当前 pattern 内的步号（0..steps-1）
        self.global_step = 0  # 从按下播放起累计的步数（供 polymeter 使用）
        self.cycle_count = 0  # pattern 循环次数（供条件触发使用）
        self.fill_now = False  # 当前循环是否是“过门”循环
        # ---- 事件队列 ----
        self.pending: List[XuEvent] = []  # 音序器产生、还没到播放时刻的事件
        self.manual: List[XuEvent] = []  # GUI 点鼓垫产生的事件
        self._rand = 0x808808  # xorshift32 状态，确定性随机
        # 外部时钟模式：为 True 时内部不再自己推进步进，改由 MIDI Clock 驱动
        self.clock_external = False
        self._clock_pulse = 0  # MIDI 时钟计数（24 PPQN，6 个=1 个 16 分）
        # ---- Pattern / Song ----
        self.pending_pattern: Optional[int] = None  # 待切换的 pattern 编号
        self.song_index = 0  # 歌曲链当前第几节
        self.song_repeat = 0  # 当前节已重复次数
        # ---- 主效果状态（必须跨块保留，否则每块都会“啪”一声）----
        from .dsp import XuBiquad  # 延迟导入，避免循环依赖
        # 左右声道必须各用各的滤波器对象：共用一个会把两次 process 的
        # 延迟线状态串在一起，造成左右声道互相“污染”，这是徐某调试时
        # 用示波器看出来的隐蔽 bug。
        self.lp_l = XuBiquad(sr)
        self.lp_r = XuBiquad(sr)
        self.hp_l = XuBiquad(sr)
        self.hp_r = XuBiquad(sr)
        self._lp_key = (0.0, 0.0)  # 上次低通参数，变化时才重算系数
        self._hp_key = (0.0, 0.0)
        # ---- 统计快照 ----
        self.peak = 0.0
        self.meter_l = 0.0
        self.meter_r = 0.0
        self.xruns = 0
        self.cpu = 0.0
        self.last_block_time = 0.0
        # ---- 设备 ----
        self.stream = None  # sounddevice.OutputStream，start() 后才有
        self.midi = None  # 可选的 MIDI 输入
        self.midi_events: List[Tuple[int, int, int]] = []  # (status, d1, d2)

    # ==================================================================
    # 传输控制
    # ==================================================================
    def play(self) -> None:
        """开始走带：把当前块起点当作第 0 步，绝不等待。"""
        if self.playing:
            return
        self.playing = True
        self.step_index = 0
        self.global_step = 0
        self.cycle_count = 0
        self.fill_now = False
        self.song_index = 0
        self.song_repeat = 0
        # 立即从当前样本位置开始，next_step 与播放起点对齐
        self.next_step_abs = self.sample_pos
        self.pending.clear()

    def stop(self) -> None:
        """
        停止走带并**立刻掐断所有正在发声的 voice**。

        徐某特别强调这一点：停止不是“不再触发”，而是“现在就安静”。
        否则长衰减的吊镲会继续响两三秒，用户会以为程序卡了。
        """
        self.playing = False
        now = self.sample_pos
        for pool in self.voices:
            for v in pool:
                # 10ms 淡出：既“说停就停”，又不会在扬声器里留下“咔哒”声
                v.kill(now, fade=480)
        self.pending.clear()

    def toggle(self) -> None:
        """空格键调用：在播放与停止之间切换。"""
        if self.playing:
            self.stop()
        else:
            self.play()

    # ==================================================================
    # 设备管理
    # ==================================================================
    def start_audio(self) -> None:
        """
        打开 PortAudio 输出流。

        为什么用 OutputStream 而不是 play()：前者是基于回调的流，
        PortAudio 在自己的高优先级线程里主动来取数据，时序由声卡驱动；
        后者是“喂数据”模式，延迟高且受 Python 调度影响。
        """
        import sounddevice as sd  # 延迟导入：没装声卡库时 UI 仍能启动
        if self.stream is not None:
            return
        self.stream = sd.OutputStream(
            samplerate=self.sr,
            blocksize=BLOCK_SIZE,
            channels=2,
            dtype="float32",
            latency="low",  # 让 PortAudio 选择它认为的低延迟
            callback=self._callback,
        )
        self.stream.start()

    def stop_audio(self) -> None:
        """关闭设备：先停流再置空，保证窗口关闭后没有残留音频线程。"""
        if self.stream is not None:
            try:
                self.stream.stop()
                self.stream.close()
            finally:
                self.stream = None

    # ==================================================================
    # 音频回调（实时线程）
    # ==================================================================
    def _callback(self, outdata: np.ndarray, frames: int,
                  time_info, status) -> None:
        """
        PortAudio 回调入口。

        注意：这个函数在 C 音频线程里被调用，任何异常都会导致流中止，
        因此用 try/except 兜底：出错时输出静音并累计 xrun，绝不让程序崩溃。
        """
        t0 = time.perf_counter()  # 仅用于统计，不参与音频时序
        if status:
            self.xruns += 1  # 欠载/溢出计数，UI 可以显示出来
        try:
            block = self.render_block(frames)
            outdata[:] = block
        except Exception:
            outdata.fill(0.0)
            self.xruns += 1
        # CPU 占用：本块渲染耗时 / 本块应有的时长，做成慢速平滑
        dt = time.perf_counter() - t0
        ideal = frames / float(self.sr)
        self.cpu = self.cpu * 0.95 + (dt / max(ideal, 1e-6)) * 0.05

    # ==================================================================
    # 事件调度
    # ==================================================================
    def _xorshift(self) -> int:
        """
        32 位 xorshift 随机数。

        为什么不用 random.random()：规格书要求“确定性随机源”，
        同一个工程、同一个种子，每次播放的概率触发必须一致，
        否则导出和实时会不一样。xorshift 只用了三次移位异或，极快且无分配。
        """
        x = self._rand & 0xFFFFFFFF
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        self._rand = x & 0xFFFFFFFF
        return self._rand

    def _step_len(self) -> int:
        """当前 BPM 下一个 16 分步的样本数。用 round 消除累积误差。"""
        bpm = min(max(float(self.project.master.bpm), 40.0), 300.0)
        return max(64, int(round(self.sr * 60.0 / bpm / 4.0)))

    def _cond_ok(self, cond: int) -> bool:
        """判断条件触发是否放行。fill_now 与 cycle_count 都在音频线程内维护。"""
        if cond == COND_ALWAYS:
            return True
        if cond == COND_FILL:
            return self.fill_now
        if cond == COND_NOT_FILL:
            return not self.fill_now
        if cond == COND_EVERY_2:
            return (self.cycle_count % 2) == 0
        if cond == COND_EVERY_4:
            return (self.cycle_count % 4) == 0
        return True

    def _emit_step(self, step_abs: int) -> None:
        """
        展开一个 16 分步：把每轨的 on/probability/ratchet/micro/swing
        全部换算成“绝对样本时刻”的事件，压入 pending。
        """
        pat = self.project.patterns[self.project.current_pattern]
        step_len = self._step_len()
        # Swing：奇数步向后偏移。0.5 是经验值，让 50% swing 听起来像三连音
        swing_off = 0
        if (self.step_index % 2) == 1:
            swing_off = int(step_len * float(self.project.master.swing) * 0.5)
        base = step_abs + swing_off
        # 每轨步号：支持 polymeter（轨道长度独立于 pattern 总步数）
        for pad in range(PAD_COUNT):
            tl = max(1, min(64, pat.track_len[pad]))
            idx = self.global_step % tl
            st = pat.data[pad][idx]
            if not st.on:
                continue
            if not self._cond_ok(st.cond):
                continue
            # 概率判定：100% 直接放行，0% 直接跳过，避免浪费随机数序列
            if st.prob <= 0:
                continue
            if st.prob < 100:
                if (self._xorshift() % 100) >= st.prob:
                    continue
            # 力度归一化；重音额外 +30%，并在合成器里改变音色
            vel = max(0.02, min(1.0, st.velocity / 127.0))
            if st.accent:
                vel = min(1.25, vel * 1.30)
            # Ratchet：一步切成 N 个采样精确的子事件
            r = max(1, min(8, st.ratchet))
            for i in range(r):
                t = base + st.micro + int(i * step_len / r)
                # 子事件力度按 8% 递减，听起来像自然的“滚奏收尾”
                v = vel * (0.92 ** i)
                self.pending.append((t, pad, v, bool(st.accent)))

    def _advance_after_step(self) -> None:
        """
        一步结束后的状态推进：步号、循环、pattern 切换、歌曲链。

        所有“切歌”动作都发生在这里，也就是循环边界；
        绝不允许在一步中间换 pattern，否则 listeners 会听到半个小节。
        """
        pat = self.project.patterns[self.project.current_pattern]
        self.step_index += 1
        self.global_step += 1
        if self.step_index < max(1, pat.steps):
            return
        # ---- 一个循环结束 ----
        self.step_index = 0
        self.cycle_count += 1
        # 每 4 个循环的最后一个是 fill 循环（供 fill 条件使用）
        self.fill_now = (self.cycle_count % 4) == 3
        # 1) 手动请求的 pattern 切换优先
        if self.pending_pattern is not None:
            self.project.current_pattern = max(
                0, min(len(self.project.patterns) - 1, self.pending_pattern))
            self.pending_pattern = None
            self.song_repeat = 0
            return
        # 2) Song 模式：按链推进
        if self.project.master.song_mode and self.project.song.entries:
            entry = self.project.song.entries[self.song_index % len(self.project.song.entries)]
            self.song_repeat += 1
            if self.song_repeat >= max(1, entry.repeats):
                self.song_repeat = 0
                self.song_index = (self.song_index + 1) % len(self.project.song.entries)
                nxt = self.project.song.entries[self.song_index]
                self.project.current_pattern = max(
                    0, min(len(self.project.patterns) - 1, nxt.pattern))

    def _collect_events(self, b0: int, b1: int) -> List[XuEvent]:
        """
        取出本块内所有事件，按时间排序。

        除了 pending（音序器）和 manual（手点），这里还负责“补齐”
        已经过期的步进：如果系统卡顿导致 next_step_abs 落在过去，
        立即补发而不是跳过，保证 rhythm 不断拍。
        """
        events: List[XuEvent] = []
        # 手点事件：原子地“换出”整个列表，避免和 GUI 线程同时 append
        manual, self.manual = self.manual, []
        for ev in manual:
            # 手点事件通常是“现在+32 样本”，但极少数情况下会落到下一块，
            # 这时必须放回队列，绝不能提前触发，否则听起来像抢拍。
            if ev[0] < b1:
                events.append((max(b0, ev[0]), ev[1], ev[2], ev[3]))
            else:
                self.manual.append(ev)
        # 走带：把本块内到期的步进全部展开
        if self.playing and not self.clock_external:
            guard = 0
            while self.next_step_abs < b1 and guard < 64:
                guard += 1
                self._emit_step(max(b0, self.next_step_abs))
                self.next_step_abs += self._step_len()
                self._advance_after_step()
        # 从 pending 里挑出本块内的
        remain: List[XuEvent] = []
        for ev in self.pending:
            if ev[0] < b1:
                events.append((max(b0, ev[0]), ev[1], ev[2], ev[3]))
            else:
                remain.append(ev)
        self.pending = remain
        events.sort(key=lambda e: e[0])
        return events

    # ==================================================================
    # 发声
    # ==================================================================
    def _audible(self, pad: int) -> bool:
        """
        判断某轨当前是否应该出声（mute/solo）。

        徐某把它放在混音前而不是触发前：即使静音，voice 仍然照常推进，
        取消静音时声音会从“当前年龄”继续，不会突然从头发声，也不会
        因为静音而堆积一堆等待释放的 voice。
        """
        solo_any = any(p.solo for p in self.project.pads)
        p = self.project.pads[pad]
        if p.mute:
            return False
        if solo_any and not p.solo:
            return False
        return True

    def _trigger(self, pad: int, when: int, vel: float, accent: bool) -> None:
        """
        在 when 时刻触发一个 voice，并处理 choke 与 voice stealing。
        """
        pool = self.voices[pad]
        # 1) 闭镲/开镲互斥组：闭镲响起时必须掐断开镲（808 的经典行为）
        if pad == 15:  # CH 闭镲
            for v in self.voices[14]:
                v.kill(when)
        elif pad == 14:  # OH 开镲：重新触发时先掐断旧的，避免叠加
            for v in self.voices[14]:
                v.kill(when)
        # 2) 找空闲 voice
        target: Optional[XuDrumVoice] = None
        for v in pool:
            if not v.active:
                target = v
                break
        # 3) 没有空闲就抢占最老的那个（voice stealing）
        if target is None:
            target = min(pool, key=lambda v: v.start)
            target.kill(when)
        # 4) 触发
        target.trigger(when, vel, accent)
        # 5) 触发后如果该轨被静音，仍然让 voice 走完，只是混音时被跳过

    def _render_voices(self, out: np.ndarray, b0: int, a: int, b: int) -> None:
        """
        把 [a,b) 区间内所有 voice 的声音叠加到 out。

        out 的索引以 b0 为原点，所以传给 voice 的仍是全局绝对样本 a/b。
        """
        for pad in range(PAD_COUNT):
            if not self._audible(pad):
                # 静音/未独奏：跳过混音，但 voice 的时间仍在推进（见 render 内 start 基准）
                continue
            p = self.project.pads[pad]
            for v in self.voices[pad]:
                if v.active:
                    v.render(out, b0, b, p)

    # ==================================================================
    # 主渲染
    # ==================================================================
    def render_block(self, frames: int) -> np.ndarray:
        """
        渲染一块立体声 float32。

        流程严格按规格书第 6 节：
          voices 求和 → 每轨静音/独奏（已在 _render_voices 内）→
          高通 → 低通 → 过载 → 软限幅 → 主音量
        """
        b0 = self.sample_pos
        b1 = b0 + frames
        out = np.zeros((frames, 2), dtype=np.float32)
        events = self._collect_events(b0, b1)
        # 按事件时刻切段，逐段渲染 + 在段首触发新 voice
        cur = b0
        for ev in events:
            t, pad, vel, accent = ev
            if t > cur:
                self._render_voices(out, b0, cur, t)
                cur = t
            if t < b1:
                self._trigger(pad, t, vel, accent)
        if cur < b1:
            self._render_voices(out, b0, cur, b1)

        # ---- 主效果链 ----
        m = self.project.master
        # 截止频率只在变化超过 0.5Hz 时重算系数，避免每块都算三角函数
        if m.hp_on:
            key = (float(m.hp_cutoff), float(m.hp_q))
            if key != self._hp_key:
                self.hp_l.set_highpass(key[0], key[1])
                self.hp_r.set_highpass(key[0], key[1])
                self._hp_key = key
            out[:, 0] = self.hp_l.process(out[:, 0])
            out[:, 1] = self.hp_r.process(out[:, 1])
        if m.lp_on:
            key = (float(m.lp_cutoff), float(m.lp_q))
            if key != self._lp_key:
                self.lp_l.set_lowpass(key[0], key[1])
                self.lp_r.set_lowpass(key[0], key[1])
                self._lp_key = key
            out[:, 0] = self.lp_l.process(out[:, 0])
            out[:, 1] = self.lp_r.process(out[:, 1])
        # 过载：tanh(x*drive)/tanh(drive)，drive=1 时接近线性
        drive = min(max(float(m.drive), 1.0), 8.0)
        if drive > 1.001:
            norm = math.tanh(drive)
            out = np.tanh(out * drive) / norm
        # 软限幅：x/(1+|x|)，无论怎么叠加都不会超过 ±1
        out = out / (1.0 + np.abs(out))
        # 主音量
        vol = min(max(float(m.volume), 0.0), 1.0)
        out *= vol
        # 统计电平（只在音频线程写，UI 线程读，天然安全）
        if frames:
            pl = float(np.max(np.abs(out[:, 0]))) if out.shape[0] else 0.0
            pr = float(np.max(np.abs(out[:, 1]))) if out.shape[0] else 0.0
            # 峰值表带 0.75 的衰减，看起来像真正的 VU
            self.meter_l = max(pl, self.meter_l * 0.75)
            self.meter_r = max(pr, self.meter_r * 0.75)
            self.peak = max(self.meter_l, self.meter_r)

        # 块结束后推进全局样本钟
        self.sample_pos = b1
        # 顺带回收早已结束的 voice（防止 kill 后一直占着 active 标志）
        for pool in self.voices:
            for v in pool:
                if v.active and b0 - v.start > 4 * self.sr:
                    v.active = False
        return out

    # ==================================================================
    # 供 GUI 调用的接口
    # ==================================================================
    def trigger_pad(self, pad: int, velocity: int = 112, accent: bool = False) -> None:
        """GUI 点击鼓垫：把事件塞进 manual 队列，由音频线程在下一块开头消费。"""
        when = self.sample_pos + 32  # 32 样本的“安全提前量”，避免贴脸
        self.manual.append((when, int(pad), max(0.02, velocity / 127.0), bool(accent)))

    def request_pattern(self, index: int) -> None:
        """请求切换 pattern；真正的切换发生在下一个循环边界。"""
        if self.playing:
            self.pending_pattern = int(index)
        else:
            self.project.current_pattern = max(0, min(len(self.project.patterns) - 1, index))

    def external_clock_tick(self) -> None:
        """
        外部 MIDI 时钟：每收到 24 PPQN 中的一个脉冲就调用一次。

        每 6 个脉冲 = 1 个 16 分音符 → 推进一个步进。
        这里用“当前样本位置”作为触发点，精度受 winmm 事件到达时间限制，
        但音序本身仍由样本钟表达，不会因为 UI 卡顿而漂移。
        """
        if not self.playing:
            return
        self._clock_pulse += 1
        if self._clock_pulse < 6:
            return
        self._clock_pulse = 0
        self._emit_step(self.sample_pos)
        self._advance_after_step()
        self.next_step_abs = self.sample_pos + self._step_len()

    def snapshot(self) -> Dict[str, float]:
        """
        给 UI 的只读快照。

        为什么返回普通 dict 而不是共享对象：UI 每 30ms 读一次，
        新建一个小 dict 的开销可以忽略，但能彻底避免 UI 读到一半被音频线程改掉。
        """
        return {
            "playing": 1.0 if self.playing else 0.0,
            "step": float(self.step_index),
            "pattern": float(self.project.current_pattern),
            "meter_l": self.meter_l,
            "meter_r": self.meter_r,
            "xruns": float(self.xruns),
            "cpu": self.cpu,
            "sample_pos": float(self.sample_pos),
            "cycle": float(self.cycle_count),
        }

    # ==================================================================
    # 离线渲染 / 导出
    # ==================================================================
    def _fresh_copy(self, solo_pad: Optional[int] = None) -> "XuEngine":
        """
        为离线渲染创建一个独立引擎。

        为什么复制工程：离线渲染要改 solo 状态（分轨导出），
        绝不能影响用户正在听的实时工程。deepcopy 只在导出瞬间做一次，
        不在音频线程里，代价可以接受。
        """
        proj = copy.deepcopy(self.project)
        # 离线时关闭 song 模式以外的一切“现场状态”
        proj.current_pattern = self.project.current_pattern
        if solo_pad is not None:
            for i, p in enumerate(proj.pads):
                p.solo = (i == solo_pad)
                p.mute = False
        eng = XuEngine(proj, self.sr)
        eng.playing = True
        eng.step_index = 0
        eng.global_step = 0
        eng.cycle_count = 0
        eng.next_step_abs = 0
        return eng

    def _render_seconds(self, seconds: float) -> np.ndarray:
        """按固定块推进离线引擎，返回立体声 float32 浮点缓冲。"""
        total = int(seconds * self.sr)
        chunks: List[np.ndarray] = []
        done = 0
        while done < total:
            n = min(BLOCK_SIZE, total - done)
            chunks.append(self.render_block(n).copy())  # copy 因为下块会复用吗？不会，但保持独立
            done += n
        if not chunks:
            return np.zeros((0, 2), dtype=np.float32)
        return np.concatenate(chunks, axis=0)

    def render_pattern(self, repeats: int = 1) -> np.ndarray:
        """
        离线渲染当前 pattern（重复 repeats 遍），末尾加 3 秒尾音。

        步长按当前 BPM 计算，因此导出速度与实时完全一致。
        """
        eng = self._fresh_copy()
        pat = eng.project.patterns[eng.project.current_pattern]
        step_len = eng._step_len()
        seconds = (pat.steps * max(1, repeats) * step_len) / self.sr + 3.0
        return eng._render_seconds(seconds)

    def render_song(self) -> np.ndarray:
        """离线渲染整条歌曲链（一遍），末尾加 3 秒尾音。"""
        eng = self._fresh_copy()
        total_steps = 0
        for e in eng.project.song.entries:
            idx = max(0, min(len(eng.project.patterns) - 1, e.pattern))
            total_steps += eng.project.patterns[idx].steps * max(1, e.repeats)
        if total_steps == 0:
            total_steps = eng.project.patterns[eng.project.current_pattern].steps
        seconds = total_steps * eng._step_len() / self.sr + 3.0
        return eng._render_seconds(seconds)

    @staticmethod
    def write_wav(path: str, audio: np.ndarray, sr: int = SAMPLE_RATE) -> None:
        """
        写 16bit PCM 立体声 WAV。

        知识点：int16 的范围是 -32768..32767，浮点 ±1.0 要乘 32767。
        写之前再做一次软限幅，防止某些极端参数把样本顶爆后回绕成噪音。
        """
        x = np.clip(audio, -1.0, 1.0)
        pcm = (x * 32767.0).astype(np.int16)
        with wave.open(path, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(pcm.tobytes())

    def export_wav(self, path: str, mode: str = "pattern", repeats: int = 1) -> None:
        """导出 WAV；mode='pattern' 或 'song'。"""
        audio = self.render_song() if mode == "song" else self.render_pattern(repeats)
        self.write_wav(path, audio, self.sr)

    def export_stems(self, folder: str, prefix: str = "xu808") -> List[str]:
        """
        分轨导出：每个声部单独渲染一个 WAV。

        实现方式是对副本设置 solo，仍然走同一条 DSP 路径，
        因此分轨之和与主输出的差别只在于主滤波/过载（规格书允许）。
        """
        import os
        out: List[str] = []
        for pad in range(PAD_COUNT):
            eng = self._fresh_copy(solo_pad=pad)
            pat = eng.project.patterns[eng.project.current_pattern]
            seconds = pat.steps * eng._step_len() / self.sr + 2.5
            audio = eng._render_seconds(seconds)
            path = os.path.join(folder, f"{prefix}_stem_{pad:02d}.wav")
            self.write_wav(path, audio, self.sr)
            out.append(path)
        return out


# ---------------------------------------------------------------------------
# MIDI 文件导出（不依赖任何第三方库）
# ---------------------------------------------------------------------------
def _vlq(value: int) -> bytes:
    """
    MIDI 变长量（Variable Length Quantity）。

    知识点：MIDI 用 7 位一组表示长度，最高位是“还有后续”标志。
    例如 0x80 写成 0x81 0x00。徐某第一次写 SMF 时忘了这点，
    Cubase 直接拒绝导入，折腾了半宿。
    """
    value = max(0, int(value))
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def export_midi(project: XuProject, path: str, mode: str = "pattern",
                repeats: int = 1) -> None:
    """
    导出标准 MIDI 文件（SMF Format 1，PPQ=480）。

    规格书规定：16 分音符 = 120 tick；微时偏移按样本换算成 tick。
    """
    ppq = 480
    sr = SAMPLE_RATE
    bpm = min(max(float(project.master.bpm), 40.0), 300.0)
    step_len = sr * 60.0 / bpm / 4.0  # 一个 16 分的样本数
    # 事件收集：(tick, note, velocity)
    notes: List[Tuple[int, int, int]] = []
    # 决定要导出哪些 pattern 以及顺序
    if mode == "song" and project.song.entries:
        seq: List[int] = []
        for e in project.song.entries:
            seq.extend([max(0, min(len(project.patterns) - 1, e.pattern))] * max(1, e.repeats))
    else:
        seq = [project.current_pattern] * max(1, repeats)

    cursor_steps = 0  # 已经走过的 16 分步数
    for pat_idx in seq:
        pat = project.patterns[pat_idx]
        for local in range(pat.steps):
            step_len_now = step_len
            swing = int(step_len_now * project.master.swing * 0.5) if (local % 2) else 0
            for pad in range(PAD_COUNT):
                tl = max(1, min(64, pat.track_len[pad]))
                idx = (cursor_steps + local) % tl
                st = pat.data[pad][idx]
                if not st.on:
                    continue
                if st.prob <= 0:
                    continue
                # 条件/概率在 MIDI 导出时按“全部放行”处理：MIDI 是数据，
                # 不该把某一次随机结果固化进去；概率信息保留在工程文件里。
                vel = max(1, min(127, int(st.velocity * (1.3 if st.accent else 1.0))))
                base_tick = int((cursor_steps + local) * (ppq / 4.0))  # 16分=120tick
                micro_tick = int(round(st.micro / sr * (bpm / 60.0) * ppq))
                r = max(1, min(8, st.ratchet))
                for i in range(r):
                    t = base_tick + int(swing / step_len_now * (ppq / 4.0)) + micro_tick
                    t += int(i * (ppq / 4.0) / r)
                    notes.append((max(0, t), PAD_MIDI_NOTES[pad], vel))
        cursor_steps += pat.steps

    notes.sort(key=lambda x: (x[0], x[1]))
    # ---- 构造 SMF ----
    track = bytearray()
    # 轨道名
    name = b"XU-808 by XuMou"
    track += _vlq(0) + b"\xff\x03" + _vlq(len(name)) + name
    # 速度：微秒/四分音符 = 60,000,000 / BPM
    tempo = int(60_000_000 / bpm)
    track += _vlq(0) + b"\xff\x51\x03" + tempo.to_bytes(3, "big")
    # 拍号：分子/分母（分母是 2 的幂次）
    track += _vlq(0) + b"\xff\x58\x04" + bytes([project.master.beats_per_bar,
                                                int(math.log2(max(1, project.master.beat_unit))),
                                                24, 8])
    last_tick = 0
    for tick, note, vel in notes:
        delta = max(0, tick - last_tick)
        # Note On（通道 10 = 打击乐）
        track += _vlq(delta) + bytes([0x99, note & 0x7F, vel & 0x7F])
        # 鼓机单击即响：10 tick 后立刻 Note Off，避免 DAW 里挂住
        track += _vlq(10) + bytes([0x89, note & 0x7F, 0x40])
        last_tick = tick + 10
    track += _vlq(0) + b"\xff\x2f\x00"  # End of Track

    header = b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") \
             + (1).to_bytes(2, "big") + ppq.to_bytes(2, "big")
    chunk = b"MTrk" + len(track).to_bytes(4, "big") + bytes(track)
    with open(path, "wb") as f:
        f.write(header + chunk)
