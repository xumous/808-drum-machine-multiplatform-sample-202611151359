# XU-808 统一实现规格（所有语言版本必须一致）

> 署名：徐某。本文是所有平台版本（Python/Java/C/Qt C++/Node/Android/C#/Kotlin/JSP/JavaEE/VB/Go/Rust/PHP/ASM/Ruby）
> 的**唯一行为基准**。任何移植不得删减功能，不得改变下面的编号、语义与默认值。

## 1. 全局音频规格

| 项目    | 值                  | 说明                                           |
|-------|--------------------|----------------------------------------------|
| 采样率   | 48000 Hz           | 所有版本统一，便于跨版本 A/B 对比                          |
| 声道    | 2（立体声）             | 左右声道内容相同，声像由每轨 pan 决定                        |
| 内部处理  | float32            | 输出前做软限幅                                      |
| 实时块大小 | 256 / 512 样本       | Python 512、C#/C/Qt 256、Java 512、Electron 128 |
| 主时钟   | 音频回调内累计 frameCount | 严禁用 UI 定时器当时钟                                |
| 时间单位  | 样本（int64）          | 所有步进/微时/ratchet 用样本计算，避免浮点漂移                 |

音频线程禁止：堆分配、加锁、文件/网络 I/O、异常抛出、UI 调用。GUI→音频参数一律通过
无锁队列/原子变量；音频→GUI 通过环形状态快照（播放头、电平、xrun 计数）。

## 2. 16 个声部（编号固定）

| #  | 代号 | 名称            | 合成模型                   |
|----|----|---------------|------------------------|
| 0  | BD | 底鼓            | 桥接 T 型正弦 + 音高包络 + 点击噪声 |
| 1  | SD | 军鼓            | 双正弦(180/330Hz) + 带通噪声  |
| 2  | LT | 低嗵鼓           | 正弦音高下滑 + 点击            |
| 3  | MT | 中嗵鼓           | 同上，基频更高                |
| 4  | HT | 高嗵鼓           | 同上，基频最高                |
| 5  | LC | 低康加           | 正弦 + 轻微噪声              |
| 6  | MC | 中康加           | 同上                     |
| 7  | HC | 高康加           | 同上                     |
| 8  | RS | 边击 Rimshot    | 1.7kHz 脉冲 + 短噪声        |
| 9  | CL | 击木 Claves     | 2.5kHz 正弦，30ms         |
| 10 | CP | 拍手 Clap       | 4 段噪声爆发 + 尾巴           |
| 11 | MA | 沙锤 Maracas    | 高通噪声，40ms              |
| 12 | CB | 牛铃 Cowbell    | 540/800Hz 双方波 + 带通     |
| 13 | CY | 吊镲 Cymbal     | 6 方波金属簇 + 高通           |
| 14 | OH | 开镲 Open Hat   | 金属簇 + 长衰减              |
| 15 | CH | 闭镲 Closed Hat | 金属簇 + 短衰减              |

分组：CH/OH/CY 属于 CHOKE 组 1（闭镲触发会掐断开镲）；其余各自独立。
每个声部最多 4 个并发 voice，超过则抢占最老的 voice（voice stealing）。

## 3. 每声部参数（0.0–1.0，映射到 DSP 物理量）

| 代号       | 参数        | 默认    | 映射                                    |
|----------|-----------|-------|---------------------------------------|
| BD       | Tune      | 0.50  | 基频 40–80Hz，音高包络终点                     |
| BD       | Decay     | 0.55  | 0.12–2.0s 指数衰减                        |
| BD       | Tone      | 0.45  | 点击噪声量与起始音高                            |
| SD       | Tune      | 0.50  | 双振荡器 150–260Hz                        |
| SD       | Snappy    | 0.55  | 噪声带通中心 1.2k–5k，噪声比                    |
| SD       | Decay     | 0.45  | 0.08–0.5s                             |
| LT/MT/HT | Tune      | 0.50  | 基频 (80/120/170Hz) × 0.6–1.6           |
| LT/MT/HT | Decay     | 0.50  | 0.15–0.9s                             |
| LT/MT/HT | Color     | 0.35  | 噪声点击量                                 |
| LC/MC/HC | Tune      | 0.50  | 基频 (220/300/400Hz) × 0.6–1.6          |
| LC/MC/HC | Decay     | 0.40  | 0.10–0.7s                             |
| RS       | Tune      | 0.50  | 1.2k–2.6kHz                           |
| RS       | Decay     | 0.35  | 20–120ms                              |
| CP       | Decay     | 0.50  | 0.1–0.6s                              |
| CP       | Tone      | 0.50  | 带通 800–2.5kHz                         |
| CP       | Tail      | 0.45  | 尾巴噪声量                                 |
| MA       | Decay     | 0.35  | 20–150ms                              |
| MA       | Tone      | 0.55  | 高通 4k–9k                              |
| CB       | Tune      | 0.50  | 400–700Hz 根音                          |
| CB       | Decay     | 0.40  | 0.08–0.8s                             |
| CY/OH/CH | Decay     | 0.45  | CH 0.03–0.25s；OH 0.1–0.8s；CY 0.2–2.0s |
| CY/OH/CH | Tone      | 0.50  | 金属簇带通 6k–12k                          |
| 全部       | Level     | 0.75  | 0–1.25 线性增益                           |
| 全部       | Pan       | 0.50  | 0=左，1=右                               |
| 全部       | Mute/Solo | false | 在混音阶段以原子标志阻断                          |

参数变化在音频线程内做一阶平滑（系数 0.002/样本），避免 zipper noise。

## 4. 步进音序器

- 每个 Pattern 长度 1–64 步，默认 16；默认 4/4，每步为 16 分音符。
- 每轨可独立设置 `trackLength`（1–64）实现 polymeter：轨道步号 = 全局步号 % trackLength。
- 每步每轨属性：
    - `on`（bool）
    - `velocity`（1–127，默认 100）
    - `accent`（bool，默认 false）
    - `probability`（0–100，默认 100）
    - `micro`（−2400..+2400 样本，默认 0）
    - `ratchet`（1–8，默认 1）
    - `condition`（ALWAYS / FILL / NOT_FILL / EVERY_2 / EVERY_4）
- Swing：0–75%。奇数 16 分步（1,3,5…）向后偏移 `swing * stepSamples * 0.5`，与 micro 叠加。
- 概率：音频线程内 xorshift32 确定性随机（种子来自工程），每步触发时刻判定一次。
- Ratchet：把一步均分为 N 个子事件，子事件时刻 = 步起点 + i*stepSamples/N，音高不随 i 改变，力度随 i 递减 8%。
- 微时偏移以样本为单位，正数延后；触发时刻 = stepStart + swingOffset + micro。
- 重音：在合成层同时影响音量（+35%）、包络时长（+20%）与音色（滤波/点击），不是单纯乘增益。
- Pattern 切换在**下一步边界**生效；播放中切歌不得截断当前步。
- Song 模式：`[{pattern, repeats}]` 列表，按顺序播放，结束后循环。

## 5. 传输与节拍

- BPM 40–300，默认 120；步长样本 = 48000 * 60 / BPM / 4。
- BPM 改变不重置播放位置，只改变下一步的间隔。
- Tap Tempo：记录最近 8 次点击间隔，去掉最大最小后平均；异常值（<30 BPM 或 >400 BPM）丢弃。
- 拍号支持 1/4–16/4 与 1/8–16/8；Pattern 步数独立于拍号。

## 6. 效果链（主输出，顺序固定）

1. 各 voice 求和（含 pan）→ 2. 每轨静音/独奏 → 3. 高通滤波（20–800Hz，可关）
   → 4. 低通滤波（200Hz–20kHz，Q 0.5–8）→ 5. 过载 `tanh(x*drive)/tanh(drive)`，drive 1–8
   → 6. 软限幅 `x/(1+|x|)`（防削波）→ 7. 主音量。
   滤波器为 RBJ 双二阶（biquad），系数在音频线程按块更新并做平滑；截止频率变化不得爆音。

## 7. 导出

- **WAV**：16bit / 48kHz / 立体声，PCM。离线渲染必须调用与实时完全相同的 DSP/音序代码，
  仅把时钟换成固定块推进；支持整曲（Song）或单 Pattern，支持分轨（每轨一个 WAV）。
- **MIDI**：SMF Format 1，PPQ=480，16 分音符=120 tick。音高映射：
  BD=36, SD=38, LT=45, MT=47, HT=50, LC=64, MC=62, HC=60, RS=37, CL=75,
  CP=39, MA=70, CB=56, CY=49, OH=46, CH=42。力度=velocity×(accent?1.3:1)，上限 127；
  微时偏移换算为 tick（±24 tick 内）并保留；ratchet 展开为多个音符。
- **工程文件**：JSON，`{"format":"XU808","version":1, ...}`，含 pattern、音色、混音、song、全局设置。
  未知字段忽略，缺失字段用默认值，保证前向/后向兼容。

## 8. MIDI 输入

- 独立 MIDI 线程读取，事件进入无锁队列，音频线程在块开始处消费。
- Note On：47 及以下按映射触发声部；Note Off 不处理（鼓机单击即响）。
- CC 7 = 主音量，CC 74 = 低通截止，CC 71 = 共鸣。
- 外部时钟：0xF8 每 24 PPQN 推进；收到 0xFA 开始、0xFC 停止。
- Program Change：切换 Pattern（0–63）。

## 9. UI 规格（各平台视觉统一）

- 深色 808 风格：背景 #121212，面板 #1c1c1c，橙色强调 #ff7a18，网格线 #2e2e2e，
  步进亮起 #ff7a18，重音 #ffd166，播放头 #4cc9f0。
- 顶部：LOGO「XU-808」、Play/Stop、BPM 数字与滑杆、Tap、Swing、Pattern 选择、
  Song 开关、Save/Load、Export WAV/MIDI、设置。
- 中部：16×(16/32/64) 步进网格，左侧声部名与 M/S 按钮；格子单击开/关，
  右键打开步进属性（力度/重音/概率/微时/ratchet/条件）。
- 下部：每声部参数条（Tune/Decay/Tone/Snappy/Tail/Level/Pan）+ 主滤波/Drive/音量/电平表。
- 刷新率 ≥30fps，仅 UI 定时器；音频线程绝不被 UI 阻塞。
- 窗口图标：app.ico；标题「XU-808 鼓机 - 徐某」。

## 10. 各语言实现要点（硬性）

- Python：PySide6 + sounddevice(PortAudio) 回调线程，DSP 用 NumPy 向量化但事件按样本落位；
  禁止在回调中创建大对象/调用 GUI；PyInstaller 单文件 EXE。
- Java：Swing + javax.sound.sampled 独立 SourceDataLine 写线程；音序在音频线程按帧计数。
- 纯 C：Win32 窗口 + waveOut/WASAPI，静态 MinGW 编译，无运行库依赖。
- Qt C++：QAudioSink 回调模式，静态 Qt 构建单 EXE。
- Node：Electron + AudioWorklet（禁止 ScriptProcessor），DSP 在 worklet 内逐样本计算。
- Android：Java + MVC，AudioTrack 回调线程，API 28 最低，debug/release APK，release 用 1000 年有效期证书。
- C#：WinForms + winmm waveOut（零 NuGet 依赖），.NET 单文件自包含发布。
- Kotlin：Swing + javax.sound，与 Java 版音色一致。
- JSP/JavaEE：MVC（Model=引擎/音序，View=JSP，Controller=Servlet），浏览器 Web Audio 渲染，
  服务端可导出 WAV/MIDI。
- VB.NET：WinForms + winmm，与 C# 版行为一致。
- Go：Win32 syscall 窗口 + waveOut，单 EXE。
- Rust：Win32 API + waveOut，单 EXE。
- PHP：MVC 网页版，前端 Web Audio 合成，后端导出 WAV/MIDI。
- ASM：x86-64 NASM 控制台 + waveOut，DSP 用标量浮点/整数实现核心声部。
- Ruby：Tk 或 Win32 fiddle 窗口 + waveOut。

## 11. 验收清单（每个版本逐项打勾）

1. 16 声部全部可听，音色可辨，无采样循环残留（上一个音必须自然衰减并释放 voice）。
2. 同时触发 16 声部无爆音、无 xrun；voice stealing 生效。
3. 16 步网格可编辑；播放头与听觉严格同步。
4. 力度/重音/概率/微时/ratchet/swing 全部生效且为采样级。
5. Pattern 切换在步边界；Song 链可循环。
6. Mute/Solo 在音频路径生效。
7. 滤波/Drive 实时可调，无 zipper noise。
8. WAV 导出与实时听感一致；MIDI 可被 DAW 正确导入。
9. 工程保存/加载完整还原。
10. 关闭窗口后音频线程安全退出，无残留进程。
