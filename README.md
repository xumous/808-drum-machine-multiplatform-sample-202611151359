# 808 合成鼓机 · 六平台完整实现

本仓库用六种技术栈实现同一台 808 风格合成鼓机，所有版本的音序器、DSP、导出与工程格式
共用同一个 C 核心（`core808/`），保证“实时好听、导出不变样”。

## 1. 交付物

| 平台               | 可执行文件                                                                              | 音频架构                                    |
|------------------|------------------------------------------------------------------------------------|-----------------------------------------|
| 纯 C / Win32      | `dist/808C.exe`（单文件，无依赖）                                                           | WASAPI 共享模式 + 事件驱动 + MMCSS Pro Audio 线程 |
| Qt 5.14.2 (C++)  | `dist/808Qt.exe`（自解压单文件，内嵌 Qt 运行时）                                                 | QAudioSink 拉模式，首次回调提升为 MMCSS Pro Audio  |
| Python 3.12 + Tk | `dist/808Python.exe`（PyInstaller onefile）                                          | miniaudio (WASAPI) C 回调，Python 不进入音频路径  |
| Java 17 + Swing  | `dist/808Java.exe`（自解压单文件，内嵌 jlink JRE）                                            | 独立 SourceDataLine 线程 + JNI + MMCSS 提升   |
| Node / Electron  | `dist/808Node.exe`（electron-builder portable 单文件 75.3 MB，内嵌 Electron 33 + WASM 核心） | AudioWorklet + WASM（DSP 编译自同一 C 核心）     |
| Android (Java)   | `android808/dist/app-debug.apk`、`app-release.apk`（v2 签名，证书有效期至 3026 年）             | 独立 AudioTrack 线程 + JNI（NDK 27）          |

## 2. 共享核心 `core808`

- `core808.h` / `core808.c` / `core808_io.c`：C99，无第三方依赖。
- 16 个声部：Kick、Snare、Clap、Rim、Low/Mid/High Tom、Conga、Cowbell、Shaker、
  Tambourine、Crash、Ride、Open Hat、Closed Hat、Clave。
- 全部为振荡器 + 噪声 + 滤波 + 包络的实时模拟建模，不是采样回放。
- 每声部 4 复音（共 64 voice），支持 voice stealing 与开/闭镲 choke group。
- 每轨参数：Tune / Decay / Tone / Character / Level / Pan，音频线程内一阶平滑。
- 16/64 步音序器：每步 on/off、力度、重音、概率、ratchet 次数与速率、微时偏移（采样单位）、
  条件触发（1:2/1:3/1:4/FILL/NOT FILL/EVERY N）；每轨独立步长（polymeter）。
- Swing 在采样域实现（奇数步延迟 = swing × 半步长）。
- 时钟只使用累计渲染采样数，BPM 变化通过整数 + 小数余数累加，无长期漂移。
- Pattern 256 个、Song 链 512 条；pattern 切换在 pattern 边界生效。
- 全局 HP/LP（TPT SVF）、tanh 饱和 + 增益补偿、DC blocker、软限幅。
- 无锁命令队列（SPSC）连接 UI 与音频线程；音频回调零 malloc/free、零锁、零文件 I/O。
- 工程双缓冲槽位 + 原子激活，加载工程不打断音频线程。
- 导出：24-bit WAV（pattern/song）、16 轨分轨 WAV（主效果旁路）、SMF MIDI（PPQ 960）。
- MIDI 输入：Windows winmm；Note On、Program Change、时钟/Start/Stop/Continue。
- 自检：`core808/test_core.exe` 覆盖 2 小节采样级触发数、满负载 20 秒压力渲染、
  WAV/MIDI/工程存取，全部 PASS。

## 3. 构建

### 纯 C

```powershell
cd c808
windres app.rc -O coff -o res.o
gcc -O2 -std=c11 -municode -mwindows win32_808.c ..\core808\core808.c ..\core808\core808_io.c res.o `
    -I..\core808 -o 808.exe -lole32 -lwinmm -lavrt -lcomdlg32 -lcomctl32 -lgdi32 -luser32 -static -s
```

### Qt（Qt Creator 4.11.1 / Qt 5.14.2 MinGW）

```powershell
cd qt808\build
qmake ..\808qt.pro
mingw32-make -j4
```

`qt808/portable/` 为完整运行时目录；`pack808/pack.py` 将其打包为单个 EXE。

### Python

```powershell
cd python808
C:\Users\Happy\py312\python.exe -m venv .venv
.\.venv\Scripts\pip install pyinstaller
gcc -O2 -std=c11 -shared py808_audio.c ..\core808\core808.c ..\core808\core808_io.c `
    -I..\core808 -o 808core.dll -lwinmm -lole32 -static-libgcc -Wl,--export-all-symbols
.\.venv\Scripts\python -m PyInstaller --onefile --windowed --icon ..\app.ico `
    --add-binary "808core.dll;." --name 808Python main.py
```

### Java

```powershell
cd java808
gcc -O2 -std=c11 -shared native\jni808.c ..\core808\core808.c ..\core808\core808_io.c `
    -I..\core808 -I"%JDK%\include" -I"%JDK%\include\win32" -o 808jni.dll `
    -lwinmm -lole32 -lavrt -static-libgcc
javac -encoding UTF-8 -d classes src\native808\*.java
jar cfe 808.jar native808.Main -C classes .
jlink --add-modules java.base,java.desktop --output jre --strip-debug --no-header-files --no-man-pages --compress=2
gcc -O2 -municode -mwindows launcher.c -o portableJava\808Java.exe -static -s
python ..\pack808\pack.py portableJava 808Java 808Java.exe ..\dist\808Java.exe
```

`portableJava/` 内含 `launcher + jre + app(808.jar, 808jni.dll)`。

### 单文件打包器

`pack808/pack.py` 生成无压缩归档并用 `windres` 嵌入 RCDATA 资源，`selfextract.c`
首次运行时解压到 `%LOCALAPPDATA%\808Machine\<app>\`，随后启动。标记文件避免重复解压。

## 4. 功能对照

所有桌面版本均实现：

- 播放/停止、BPM 20–400、Tap Tempo、拍号循环（4/4、3/4、5/4、7/8、6/8、12/8）
- Swing、pattern 选择/长度/复制/清空、16 个快捷 pattern + 分页（共 256）
- 16 轨网格：点击切换、Ctrl 重音、Shift 概率循环、Alt ratchet 循环、滚轮力度
- 逐步属性：力度、概率、ratchet 次数、ratchet 速率、微时（±2400 采样）、条件触发、重音
- 6 个声部旋钮、6 个主控旋钮（Volume/Drive/HP/LP/Reso/Accent）
- 每轨 mute/solo、独立轨道步长（polymeter）、16 个打击垫
- Song 链编辑器（pattern + 重复次数，增删/排序）
- 工程保存/加载（版本化二进制 + FNV 校验）
- WAV、分轨 WAV、MIDI 导出（后台线程，不阻塞 UI）
- MIDI 输入设备循环切换、MIDI 时钟同步开关
- 播放头、16 轨电平、主输出电平、CPU/xrun 状态

## 5. 验证记录

- `core808/test_core.exe`：ALL PASS（0 failures）。
    - 默认 2 小节触发数精确 32 次；
    - 等功率声像回归：PAN=50% 时左右峰值 0.988 / 0.988（修复前左声道为 0）；
    - 16 轨全步进 + ratchet 压力渲染 20 秒，无 NaN/失控，渲染速度约为实时 10 倍；
    - 工程保存/加载、BPM 恢复、WAV/MIDI/分轨导出均通过。
- 纯 C：WASAPI 48 kHz / 2 ch / 32-bit，连续播放 90 秒 CPU 3%，XRUN 0，播放头正常。
- Qt：QAudioSink 48 kHz / Int16 / 21 ms 缓冲，CPU 0–1%，XRUN 0。
- Python：miniaudio/WASAPI 48 kHz，CPU 约 2%，XRUN 0；Tk 界面 33 ms 刷新。
- Java：SourceDataLine 48 kHz / 16-bit / 42 ms 缓冲，CPU 0–12%，XRUN 0（启动阶段偶发计数）。
- Node：AudioWorklet 持续回调（`F` 帧计数与 `BLK` 块计数按 48000/375 每秒递增），CPU 1–2%，XRUN 0。
- 五个桌面包均从自解压单文件 / PyInstaller onefile / electron-builder portable 启动并截图核验。
- Android：`apksigner verify` 通过（v2），证书 CN=Android808 有效期至 3026-03-18。

## 6. 架构决策与限制（明确记录）

1. **DSP 单一来源**：六端共用 `core808.c`，避免“各端音色不一致”。非 C 前端只做参数传递与 UI。
2. **Python 实时性**：音频回调完全位于 miniaudio 的 C 线程，Python 只通过 ctypes 下发命令；
   UI 采用 30 fps 定时刷新，DSP 不在 Python 层执行。
3. **Java 延迟**：Java Sound `SourceDataLine` 在 Windows 上典型 30–100 ms；本项目实测约 42 ms
   缓冲，已用 JNI 渲染 + MMCSS 线程提升降低抖动，但击打响应无法达到硬件鼓机水平（需求中已注明）。
4. **Qt 线程优先级**：QAudioSink 的拉模式回调线程默认普通优先级，首次回调时调用
   `AvSetMmThreadCharacteristics("Pro Audio")` 提升，避免 UI 重绘抢占。
5. **WASAPI 共享模式**：C 版使用事件驱动共享模式（10 ms 请求），未强制独占/ASIO；
   独占模式会阻塞其他应用，且需要设备格式协商，默认不启用。
6. **分轨导出**：16 个 WAV，主效果旁路，保留轨道电平/声像，便于 DAW 混音。
7. **MIDI 导出**：PPQ 960，鼓通道 10；ratchet 展开为连续音符，微时/Swing 折算为 tick；
   概率与条件触发不参与离线导出（保持确定性）。
8. **单文件 EXE**：Qt/Java 采用自解压到 `%LOCALAPPDATA%` 的方式，首次启动稍慢，之后直接启动；
   卸载只需删除该目录。
9. **Java 运行时分包**：使用 jlink 仅包含 `java.base` + `java.desktop`，避免携带完整 JDK。
10. **Android/Electron**：见各自目录 README，架构与构建命令在对应文件中记录。

### Node / Electron 交付细节

- Electron 33.4.11 + `app://` 自定义协议（secure context，AudioWorklet/Web MIDI 可用）。
- DSP 以 Zig 0.14.1 将 `core808.c` + `n808_glue.c` 编译为 `wasm32-wasi`（976 KB），
  在 AudioWorklet 线程内实例化并渲染；渲染进程只接收 10856 字节快照（约 30 fps）。
- 音频不可用时自动降级为渲染进程本地 WASM 引擎（静音，但编辑/导出/工程功能完整）。
- 单文件：`dist/808Node.exe`（75,289,872 B，electron-builder 24.13.3 portable 目标，
  内嵌 Electron 33.4.11 运行时 + `core808.wasm`；启动时自解压到临时目录并拉起主程序）。
  构建命令（在 `node808/` 下）：
  `npx electron-builder --win portable --config.electronDist=node_modules/electron/dist --config.directories.output=dist2`。
- 状态栏显示 `AUDIOWORKLET 48000 Hz · wasm · CPU · XRUN · F<frames> · BLK<blocks>`，
  可直接验证音频回调确实在运行。
- 支持 SYNC INT/MIDI、拍号循环、每轨 TRACK LEN、16 步网格、Song 编辑器、WAV/MIDI/分轨导出。

### Android 交付细节

- 包名 `com.guji808.android`，minSdk 28，targetSdk 33，compileSdk 33。
- native-code：`arm64-v8a`、`armeabi-v7a`、`x86_64`，`lib808core.so` 由 NDK 27 CMake 构建。
- APK：`android808/dist/app-debug.apk`（464,565 B）、`app-release.apk`（384,193 B）。
- 签名：`android808/keystore/android808-release.jks`，alias `android808`，
  密码 `808808` / `808808`，有效期 365000 天（2026-11-15 → 3026-03-18），SHA256withRSA / RSA 2048。
- 签名方案：APK Signature Scheme v2（`apksigner verify` 通过）。minSdk 28 下 v2 已全覆盖，
  因此未启用 v1（JAR 签名），也避免 Janus 漏洞面；v3/v4 未启用。
- 证书 SHA-256：`4a84f44bb7ef9b50b409fb70dae306b695ea7b1001785450e0d492262c496276`。
- 触摸交互：点击步进、长按重音、拖动旋钮/BPM/Swing、分页显示 64 步、
  Song 覆盖层、工程列表、导出到 `Android/data/com.guji808.android/files/exports/`。

## 7. 目录

```
core808/     共享 DSP + 音序器 + 导出 + 自检
c808/        纯 C Win32 + WASAPI
qt808/       Qt 5.14.2 工程与便携运行时
python808/   Tk + ctypes + miniaudio
java808/     Swing + JNI + jlink 便携包
node808/     Electron + AudioWorklet/WASM
android808/  Android Java + AudioTrack/JNI
pack808/     单文件自解压打包器
dist/        最终可执行文件
```
