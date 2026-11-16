# XU-808 鼓机 · Ruby 版（徐某）

> 目录：`xu808_all/16_ruby`
> 运行环境：Windows x64 · 内置 Ruby 4.0.9（x64-mingw32）· 48 kHz / 立体声 / 16bit
> 署名：徐某。所有 DSP、音序器、waveOut 后端、Win32 GDI 界面均为本目录内的 Ruby 源码实现。

---

## 1. 这是什么

一台完全用 Ruby 写的 808 鼓机：

- **16 个声部**全部实时合成，不播放预录采样：
  底鼓、军鼓、低/中/高嗵鼓、低/中/高康加、边击、击木、拍手、沙锤、牛铃、吊镲、开镲、闭镲。
- **采样级音序器**：16/32/64 步，Swing、力度、重音、概率、微时偏移、Ratchet、
  条件触发、每轨独立长度（polymeter）、Pattern 切换、Song 链。
- **主效果链**：高通 → 低通（RBJ biquad）→ tanh 过载 → 软限幅 → 主音量。
- **waveOut 实时输出**：6 × 512 帧缓冲（约 64 ms），独立音频线程，
  主时钟是音频采样计数，不是 UI 定时器。
- **Win32 GDI 自绘界面**：深色 808 配色，网格编辑、右键步进属性、参数滑杆、电平表、
  播放头、Mute/Solo、导出 WAV/MIDI、工程保存/加载，30 fps 刷新。
- **导出**：WAV（16bit/48k/立体声）、分轨 WAV、SMF Format 1 MIDI、JSON 工程。
- **MIDI 输入**：Note On 触发、CC7/74/71、Program Change 切 Pattern、
  0xFA/0xFC 启停、0xF8 时钟估算 BPM（可选，没设备也能正常运行）。

---

## 2. 目录结构

```
16_ruby/
├─ main.rb                     # 唯一入口：--selftest / --uismoke / 导出 / GUI
├─ run.bat                     # 一键启动（内置 Ruby，控制台/无控制台两种模式）
├─ app.ico                     # 窗口图标（与其它语言版本同一张）
├─ lib/xu808/
│  ├─ constants.rb             # 16 声部定义、音符映射、配色、全局规格
│  ├─ dsp.rb                   # Biquad + Voice：16 种鼓声的逐样本合成
│  ├─ sequencer.rb             # Step / Pattern / Sequencer（采样级调度）
│  ├─ engine.rb                # 16 轨 × 4 voice、参数平滑、效果链、播放控制
│  ├─ audio_winmm.rb           # winmm waveOut + CALLBACK_EVENT 音频线程
│  ├─ win32.rb                 # Fiddle 绑定 user32/gdi32/kernel32/winmm
│  ├─ ui_win32.rb              # GDI 自绘界面 + 消息循环 + 鼠标/键盘交互
│  ├─ wav.rb                   # WAV 写入 + 分轨渲染
│  ├─ midi.rb                  # SMF Format 1 导出
│  ├─ midi_in.rb               # midiIn 输入（可选）
│  ├─ project.rb               # JSON 工程保存/加载（format/version）
│  └─ selftest.rb              # 无人值守自检
├─ tools/ruby/                 # 便携 Ruby 4.0.9 运行时（36.3 MB，2685 个文件）
├─ selftest_ruby.wav           # 自检产物：960,044 字节
├─ selftest_ruby.mid           # 自检 MIDI：382 字节
├─ selftest_ruby.txt           # SELFTEST OK + 逐项结果
├─ selftest_ruby_project.json  # 自检工程（JSON 往返验证）
├─ xu808_export.wav            # 命令行导出的 8 秒示例（1,536,044 字节）
├─ xu808_export.mid            # 命令行导出的 MIDI 示例（351 字节）
└─ stems/                      # `--stems` 生成的 16 个分轨（示例，可删除）
```

模块调用关系：

```
main.rb
  └─ UI (ui_win32.rb)
       ├─ Engine (engine.rb)
       │    ├─ Voice/Biquad (dsp.rb)
       │    ├─ Sequencer/Pattern/Step (sequencer.rb)
       │    └─ constants.rb
       ├─ WinMMAudio (audio_winmm.rb) ── Fiddle ── winmm.dll
       ├─ MidiInput (midi_in.rb)     ── Fiddle ── winmm.dll
       ├─ Project (project.rb) ── JSON
       ├─ Wav (wav.rb)
       └─ Midi (midi.rb)
```

---

## 3. 一键运行

### 3.1 打开界面

双击 `run.bat`，或：

```bat
cd xu808_all\16_ruby
run.bat
```

`run.bat` 会优先使用 `tools\ruby\bin\rubyw.exe`，不会多弹一个黑框控制台。

### 3.2 自检（硬门槛）

```bat
run.bat --selftest
```

等价于：

```bat
tools\ruby\bin\ruby.exe main.rb --selftest
```

通过时最后一行一定是：

```
SELFTEST OK
```

### 3.3 其它命令

```bat
run.bat --play            :: 打开界面并自动播放
run.bat --export-wav      :: 无界面导出 8 秒 WAV
run.bat --export-midi     :: 无界面导出 MIDI
run.bat --stems stems     :: 16 轨分轨导出到 stems\
run.bat --uismoke         :: 窗口打开 1.8 秒后自动关闭，打印 UISMOKE OK
run.bat --version
```

---

## 4. 实际验证结果（2026-11-16）

机器：Windows 10 19045 x64，48 kHz 立体声，屏幕 1920×1080（125% 缩放，程序已 DPI-aware）。

### 4.1 自检

```text
XU-808 Ruby SelfTest  Ruby 4.0.9  x64-mingw32
OK   BD 单声部峰值=0.4171
OK   SD 单声部峰值=0.4280
...
OK   CH 单声部峰值=0.3960
OK   16 声部齐奏峰值=0.4328（软限幅内）
OK   闭镲 choke 开镲：之前 1 个活动，之后 0 个
OK   同轨连打 8 次，活动 voice=4（上限 4）
OK   底鼓自然衰减后 voice 自动释放
OK   Song 链 bars=[0, 0, 1, 0, 0, 1]（期望 A,A,B,A,A,B）
OK   WAV 大小=960044（期望 960044）
OK   WAV 非静音字节数=955512
OK   音序器渲染峰值=0.4328
OK   5 秒 @120BPM 走了 40 步（期望 40）
OK   离线渲染 5 秒耗时=3.18s（1.6x 实时）
OK   MIDI 导出=382 字节，MThd 头正确
OK   工程 JSON 往返：bpm=120.0, 第0轨第0步=true
SELFTEST OK
```

（不同负载下离线渲染耗时在 2.6–3.7 秒之间，约 1.4–1.9 倍实时。）

### 4.2 用 Python 独立校验 WAV（不是自己验自己）

```text
wav channels=2 rate=48000 width=2 frames=240000
samples=480000 peak=14181 (0.433) rms=8247.0 nonzero=479994
midi size=382 head=b'MThd' tracks=2
project format=XU808 version=1 patterns=1 bpm=120.0
```

### 4.3 其它实测

| 项目      | 命令                                   | 结果                            |
|---------|--------------------------------------|-------------------------------|
| 图形界面冒烟  | `run.bat --uismoke`                  | `UISMOKE OK`，50+ 次重绘无异常       |
| 鼠标交互    | 模拟左键点格子、右键开属性面板                      | 格子状态、TRACK 选中、属性面板均正确更新       |
| 关窗退出    | 发送 WM_CLOSE                          | 进程退出码 0，无残留                   |
| 实时播放    | `WinMMAudio#start` + 默认 pattern 10 s | `start=true`，`xrun=0/1`，正常出声  |
| 分轨导出    | `run.bat --stems stems`              | 16 个 WAV，每个 1,536,044 字节（8 s） |
| WAV 导出  | `run.bat --export-wav`               | 1,536,044 字节，峰值 0.431         |
| MIDI 导出 | `run.bat --export-midi`              | 351 字节，`MThd` + 2 个 `MTrk`    |

---

## 5. 依赖获取与便携运行时（代理实际执行过的命令）

本机原先没有 Ruby：`where.exe ruby` 找不到。

1. GitHub 不可达：
   ```powershell
   Invoke-WebRequest "https://api.github.com/repos/oneclick/rubyinstaller2/releases/latest"
   # 无法连接到远程服务器
   ```
2. npmmirror 没有 rubyinstaller 目录（404）；清华/中科大/华为镜像也没有可用的
   RubyInstaller 便携包。
3. 改用系统已有的 MSYS2（`C:\msys64`）安装 mingw64 Ruby：
   ```bat
   C:\msys64\usr\bin\pacman.exe -S --noconfirm --needed mingw-w64-x86_64-ruby
   ```
   下载阶段 `libyaml`/`zlib` 出现过 503，但缓存命中后安装成功，得到
   `ruby 4.0.9 (2026-10-23) +PRISM [x64-mingw32]`。
4. 把运行时复制进项目，做到“目标机器不需要装 Ruby”：
    - `C:\msys64\mingw64\bin\ruby.exe`、`rubyw.exe`
    - `x64-msvcrt-ruby400.dll`、`libgmp-10.dll`、`libffi-8.dll`
    - 整个 `C:\msys64\mingw64\lib\ruby`（标准库 + 内置 gem，含 Fiddle）
5. 便携性验证：设置
   `RUBYLIB=tools\ruby\lib\ruby\4.0.0;...\x64-mingw32`、
   `GEM_HOME=tools\ruby\lib\ruby\gems\4.0.0` 后，
   `require 'fiddle'` 与 `require 'json'` 均从项目内加载成功。

**没有使用任何第三方 gem**：Fiddle、JSON 都是 Ruby 自带/内置 gem。

---

## 6. 界面与操作

- **左键格子**：开/关该步；同时选中该轨，并试听一下。
- **右键格子**：打开 STEP 属性面板：力度、重音、概率、微时（样本）、Ratchet、条件。
- **点击声部名**：选中该轨并试听。
- **M / S**：静音 / 独奏，直接作用于音频混音阶段。
- **底部 TRACK 参数**：Tune / Decay / Tone / Snappy / Tail / Level / Pan（按声部不同）。
- **LEN −/+**：每轨独立步长，1–64，实现 polymeter。
- **MASTER FX**：HP 开关与截止、LP 截止与 Q、Drive、主音量、L/R 电平表。
- **顶部**：播放/停止、BPM ±、TAP、Swing ±、步数 16/32/64、Song、Fill、
  Pattern 前后/新建、保存/读取、导出 WAV/MIDI。
- **空格**：播放/停止；**ESC**：关闭属性面板；**滚轮**：调 BPM。
- 窗口图标为 `app.ico`，标题「XU-808 鼓机 - 徐某」，DPI-aware，窗口会自动夹到屏幕内。

Song 模式：第一次打开 Song 时，程序会把当前所有 pattern 串成链（每个重复 2 遍）；
只有一个 pattern 时就是循环播放。Pattern 切换在下一步边界生效。

---

## 7. DSP 实现要点

全部在 `dsp.rb`，逐样本计算，热循环用局部变量 + 4096 点正弦查表 + 整数相位，
避免在音频线程里创建对象。

| 声部       | 模型                                    |
|----------|---------------------------------------|
| BD       | 40–80 Hz 正弦 + 3–5 倍起始音高包络 + 4 ms 点击噪声 |
| SD       | 180/330 Hz 双正弦 + 带通噪声（中心 1.2–5 kHz）   |
| LT/MT/HT | 80/120/170 Hz 正弦，1.7 倍起始音高下滑 + 噪声点击   |
| LC/MC/HC | 220/300/400 Hz 正弦，1.25 倍起始音高，木质感      |
| RS       | 1.2–2.6 kHz 短脉冲 + 3 kHz 带通噪声          |
| CL       | 1.8–3.2 kHz 正弦，约 30 ms                |
| CP       | 4 段 7 ms 噪声爆发（0/10/20/30 ms）+ 可调尾巴    |
| MA       | 4–9 kHz 高通噪声                          |
| CB       | 540/800 Hz 双方波 + 1.2 kHz 带通           |
| CY/OH/CH | 6 个非谐方波金属簇 + 6 kHz 高通 + 少量高通噪声        |

每个 voice：

- 指数包络衰减到阈值自动 `active=false`，不会“响完不关”；
- 每轨最多 4 个并发 voice，超过抢占最老的；
- 闭镲触发时以 6 ms 快速淡出掐断开镲/吊镲（choke group 1）；
- 重音同时提升 35% 幅度、延长 20% 包络、抬高起始音高，不是单纯乘增益；
- 参数每块做一阶平滑（逐样本 0.002 的等效系数），避免 zipper noise。

音序器：

- 16 分音符长度用 `Rational(2880000, bpm*4)` 累加，长时间播放不漂移；
- 触发时刻 = 步起点 + Swing 偏移 + 微时偏移，全部是绝对采样数；
- Ratchet 把一步均分，子事件力度每个递减 8%；
- 概率用 xorshift32，种子随工程保存，可复现；
- 条件触发按 pattern 循环次数判断 `EVERY_2/EVERY_4`，`FILL/NOT_FILL` 受 Fill 开关控制。

---

## 8. 导出格式

- WAV：`RIFF/WAVE`，PCM 16bit、48 kHz、2 声道，44 字节头 + 样本数据。
- 分轨：每轨单独 solo 一个 `Marshal` 深拷贝引擎，复用同一套 DSP。
- MIDI：SMF Format 1，PPQ 480，16 分音符 = 120 tick；
  音高映射 BD36/SD38/LT45/MT47/HT50/LC64/MC62/HC60/RS37/CL75/CP39/MA70/CB56/CY49/OH46/CH42；
  重音力度 ×1.3，上限 127；微时换算 tick 并限制 ±24；Ratchet 展开成多个音符。
- 工程：JSON，`{"format":"XU808","version":1,...}`；缺字段用默认值，未知字段忽略。

---

## 9. 未完成项与限制（如实记录）

1. **没有生成单文件 EXE。**
   本机没有 OCRA / enclose / ruby-packer，GitHub 不可达，RubyInstaller 便携包也拿不到；
   自行用 7z/自解压壳拼一个“假单文件”不如诚实提供目录版。
   当前交付是 **`run.bat` + `tools\ruby` 便携运行时**，复制整个 `16_ruby` 目录到任何
   Windows x64 机器即可双击运行，无需安装 Ruby。
   以后有网络时可用 `gem install ocra`（或 ruby-packer）打包，但 Ruby 4.0 的兼容性需要另行验证。
2. **实时性能余量有限。**
   Ruby 无 JIT 时，16 声部密集段落离线渲染约 1.4–1.9 倍实时；实时线程偶尔出现
   `ovr`（单块渲染 > 32 ms）。6 × 512 帧（64 ms）缓冲能吸收大部分尖峰，
   但极端满负载（16 声部同时 + ratchet + 滤波/过载）仍可能听到轻微卡顿。
   UI 的 `waveOut OK · ovr N · xrun N` 会如实显示。
3. **延迟约 64 ms**（6 × 512 帧），高于 C/Qt 版本的 21–30 ms，这是 Ruby 解释执行
   与 waveOut 缓冲的折中。
4. **MIDI 外部时钟是“跟随 BPM”而非采样级从属时钟。**
   收到 0xF8 时每 24 tick 估算一次 BPM，内部仍以采样计数为权威时钟；
   Windows MIDI 时间戳本身有抖动，强行从属反而更容易不稳。
5. **Song 链没有独立编辑器。**
   引擎支持 `[{pattern,repeats}]` 列表，UI 在打开 Song 时自动把所有 pattern 各重复 2 遍；
   更复杂的链可以通过编辑工程 JSON 实现。
6. **UI 是固定像素布局**，已做 DPI 感知并自动夹到屏幕内；超小屏（<900×560）会压缩网格行高。

---

## 10. 复现验证命令

```bat
cd xu808_all\16_ruby

:: 1) 自检（必须看到 SELFTEST OK）
run.bat --selftest

:: 2) 检查 WAV 大小与非静音（PowerShell）
(Get-Item selftest_ruby.wav).Length          :: 960044
(Get-Item selftest_ruby.txt).Content          :: 包含 SELFTEST OK

:: 3) 界面冒烟
run.bat --uismoke                            :: UISMOKE OK

:: 4) 导出
run.bat --export-wav
run.bat --export-midi
run.bat --stems stems
```

---

## 11. 署名

徐某 · 2026-11-16
「代码可以慢一点，但不能假；没跑过的，不写进完成。」
