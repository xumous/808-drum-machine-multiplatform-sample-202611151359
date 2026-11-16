# XU-808 十六声部模拟鼓机 · 徐某

> 一套规格、十七种技术栈、同一个声音。
> 所有声音均由“振荡器 + 噪声 + 滤波器 + 包络”实时合成，不含任何预录制采样。
> 已构建并实测的清单见 [STATUS.md](STATUS.md)；一键复验脚本 `verify_all.ps1`。

---

## 1. 项目简介

XU-808 是徐某按 TR-808 的电路行为重新实现的软件鼓机：16 个声部、16/64 步音序器、
采样级时钟、Swing、概率、微时偏移、Ratchet、重音、Mute/Solo、主滤波与过载、
WAV/MIDI 导出、工程保存/加载、MIDI 输入与外部时钟。每个声部的衰减都是时间的
解析函数，声音到时间自然归零，**上一槌结束就释放 voice，绝不会越叠越多**。

本仓库把同一份《XU808_SPEC.md》分别落到 Python、Java、纯 C、Qt C++、Node/Electron、
Android(Java)、C#、Kotlin、JSP、JavaEE、VB.NET、Go、Rust、PHP、x86 ASM、Ruby 等版本中。
所有版本共享相同的声部编号、参数范围、步进语义与文件格式。

## 2. 模块与调用关系

```
┌──────────────┐   写参数/格子   ┌────────────────┐
│   UI 线程     │ ─────────────> │  Project 数据   │
│ (Qt/Swing/…) │                │ (Pattern/Pad)  │
└──────┬───────┘                └───────┬────────┘
       │ 33ms 定时读取快照                 │ 只读
       v                                v
┌──────────────┐                ┌────────────────┐
│ 播放头/电平表  │ <────────────── │   Engine 引擎   │
└──────────────┘   原子快照       │ 音序 + Voice 池 │
                                 └───────┬────────┘
                                         │ 每块渲染
                                         v
                                 ┌────────────────┐
                                 │ 音频回调线程     │
                                 │ waveOut/QAudio  │
                                 └────────────────┘
```

- **Model**：`Project / Pattern / Step / PadParams / Master / Song`，只存数据。
- **DSP**：`Biquad` + `Voice`，16 种声部合成公式完全一致。
- **Engine**：唯一权威时钟是音频回调里的 `samplePos`；所有事件换算成绝对样本时刻，
  再按块切段触发；主效果链顺序固定为 HP → LP → Drive → 软限幅 → 音量。
- **Audio**：独立高优先级线程/回调，禁止堆分配、锁、文件 IO、UI 调用。
- **UI**：只负责编辑数据与 30fps 刷新，绝不参与音序。

## 3. 功能清单

- 16 声部：BD、SD、LT、MT、HT、LC、MC、HC、RS、CL、CP、MA、CB、CY、OH、CH。
- 每声部 Tune/Decay/Tone/Snappy/Tail/Level/Pan，实时平滑。
- 16/64 步；每步 on、力度、重音、概率、微时、Ratchet、条件触发。
- Swing、每轨独立长度（polymeter）、歌曲链、Pattern 边界切换。
- 开/闭镲 choke、每声部 4 复音、voice stealing。
- 主 HP/LP 双二阶滤波、Drive 过载、软限幅、电平表。
- 导出 16bit/48kHz WAV、SMF Format 1 MIDI、分轨 WAV（部分版本）。
- 工程 JSON（含版本号）保存/加载。
- MIDI Note On 触发、CC7/74/71、Program Change 切 Pattern、Clock 同步。

## 4. 已验证的构建命令（Windows）

> 以下命令均在本机实际执行通过；路径按仓库根目录书写。

### Python（PySide6 + PortAudio，独立虚拟环境）

```powershell
cd 01_python_qt
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe main.py --selftest     # 输出 SELFTEST OK
.\.venv\Scripts\python.exe -m PyInstaller --noconfirm --clean --onefile --windowed ^
  --name XU808-Python --icon app.ico --add-data "app.ico;." ^
  --add-binary ".venv\Lib\site-packages\_sounddevice_data\portaudio-binaries\libportaudio64bit.dll;_sounddevice_data\portaudio-binaries" ^
  main.py
```

### Java（Swing + javax.sound + jpackage）

```powershell
cd 02_java_swing
javac -encoding UTF-8 -d out src\xu808\*.java
jar --create --file ..\dist\XU808-Java.jar --main-class xu808.XuMain -C out .
jpackage --type app-image --name XU808-Java --input out ^
  --main-jar ..\dist\XU808-Java.jar --main-class xu808.XuMain ^
  --icon app.ico --dest ..\dist\java
```

### 纯 C（Win32 + waveOut，静态链接）

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++（静态 Qt 6.8.1）

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C#（.NET WinForms，零 NuGet）

```powershell
cd 07_csharp
dotnet publish -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true ^
  -o ..\dist\csharp
```

### Node / Electron（AudioWorklet）

```powershell
cd 05_electron
npm install
npx electron .
# 打包：把 node_modules\electron\dist 复制为发行目录，
# 将 main.js/package.json/renderer/app.ico 放入 resources\app，
# 再把 electron.exe 改名为 XU808-Electron.exe 即可。
```

### PHP（MVC 网页版）

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
# 浏览器打开 http://127.0.0.1:18081/index.php
```

## 5. 目标机器零依赖验证

1. 取一个**干净**的 Windows（未装 Python/JDK/.NET/Qt/Node），只复制对应的 EXE/发行目录。
2. 双击运行；任务管理器里不应出现解释器/运行时进程（C、Qt、C# 版本）。
3. 按空格或 PLAY，应听到默认 808 律动；连续敲击同一底鼓 20 次，不应出现叠加失控。
4. 命令行执行 `XU808-*.exe --selftest`，应生成 960044 字节的 16bit/48k 立体声 WAV，
   文本标记为 `SELFTEST OK`。
5. 关闭窗口后，任务管理器不应残留音频线程/进程。

## 6. 使用说明

- 空格：播放/停止；数字 1–8：切 Pattern；Z X C V B N M A S D F G H J K L：敲鼓垫。
- 左键格子：开/关；右键格子：力度/重音/概率/微时/Ratchet/条件。
- 左侧 M/S：静音/独奏；点击声部名可试听。
- 底部滑杆：主低通/共鸣/高通/过载/音量；电平表实时显示。
- 保存工程为 `.xu808`（JSON）；导出 WAV/MIDI 用底部按钮。

## 7. 实时实现要点

- 时钟：`samplePos`（int64 样本）是唯一权威，UI 定时器只刷新画面。
- 事件：步进、Swing、微时、Ratchet 全部换算成绝对样本时刻。
- Choke：闭镲触发会掐断开镲；掐断带 10ms 淡出，避免爆音。
- Voice 回收：自然衰减完或被抢占后立即失活；4 秒硬上限兜底。
- 主效果：左右声道各一套 biquad 状态，避免串扰。
- 离线导出复用同一 `render()`，保证“实时好听、导出不变样”。

## 8. 常见问题

- **没有声音**：先在系统音量里确认默认输出设备；Java/Electron 版本请检查设备权限。
- **延迟偏大**：Java Sound 与 Electron 受系统音频栈限制；C/Qt/C# 版本缓冲更小。
- **杀毒误报**：单文件打包器可能被误判，请加入白名单或使用源码运行。
- **MIDI 无设备**：插上设备后重启程序；Electron 需允许 Web MIDI 权限。

## 9. 目录结构

```
00_shared/        统一规格 XU808_SPEC.md
01_python_qt/     Python + PySide6 + PortAudio
02_java_swing/    Java + Swing + javax.sound
03_c_win32/       纯 C + Win32 + waveOut
04_qt_cpp/        Qt C++ + 静态 Qt
05_electron/      Electron + AudioWorklet
06_android_java/  Android Java (MVC)
07_csharp/        C# WinForms
08_kotlin/        Kotlin Swing
09_jsp/           JSP MVC 网页版
10_javaee/        JavaEE MVC 网页版
11_vbnet/         VB.NET WinForms
12_go/            Go + Win32
13_rust/          Rust + Win32
14_php/           PHP MVC 网页版
15_asm/           x86-64 NASM
16_ruby/          Ruby + Win32
dist/             已构建的 EXE / APK / WAV / MIDI
docs/             多语言 README（md/docx/ppt）
```

## 10. 署名与许可

作者：**徐某**。代码中的类名、变量名与注释都保留了“徐某”的写作痕迹。
本项目用于学习与个人创作；TR-808 是 Roland 的商标，本项目与 Roland 无关。
