# XU-808 交付与验证状态（徐某）

> 本文件只记录**实际执行过**的构建与验证，不写“应该能跑”。
> 验证日期：2026-11-16 · Windows 10 19045 · 48kHz 立体声

## 1. 已构建并通过验证的版本

| 版本                           | 目录                | 产物                                                                           | 验证方式与结果                                                                                                                                                                                                                                                                   |
|------------------------------|-------------------|------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Python (PySide6+PortAudio)   | `01_python_qt`    | `dist/XU808-Python.exe`（93.3 MB，单文件，app.ico）                                 | `--selftest` → `SELFTEST OK`；WAV 960044 字节；停止后尾音 1e-6                                                                                                                                                                                                                     |
| Java (Swing+javax.sound)     | `02_java_swing`   | `dist/XU808-Java.jar`、`dist/java/XU808-Java/XU808-Java.exe`（app-image，含 JRE） | 自检 240000 帧 / peak 0.373 / WAV 960044；GUI 启动存活                                                                                                                                                                                                                            |
| 纯 C (Win32+waveOut)          | `03_c_win32`      | `dist/XU808-C.exe`（576 KB，静态链接）                                              | `--selftest` → SELFTEST OK；RIFF/WAVE 校验通过；GUI 启动存活                                                                                                                                                                                                                        |
| Qt C++ (静态 Qt 6.8.1)         | `04_qt_cpp`       | `dist/XU808-Qt.exe`（22.6 MB，无 Qt DLL）                                        | `--selftest` → SELFTEST OK；WAV 960044；GUI 启动存活                                                                                                                                                                                                                            |
| Node/Electron (AudioWorklet) | `05_electron`     | `dist/electron/XU808-Electron-win32-x64/`（含 `XU808-Electron.exe`）            | 打包 EXE 启动 8 秒无异常、无 JS 报错；Worklet 加载成功                                                                                                                                                                                                                                     |
| Android (Java, MVC, API 28)  | `06_android_java` | `dist/android/XU808-debug.apk`、`XU808-release.apk`（各约 72.8 KB）               | `aapt dump badging` 包名/ minSdk 28 正确；`apksigner verify` v3 通过；JVM 离线 DSP 自检 16/16 声部非静音；**未上真机**                                                                                                                                                                          |
| C# (.NET WinForms)           | `07_csharp`       | `dist/csharp/XU808-CSharp.exe`（50 MB，单文件自包含）                                 | `--selftest` → SELFTEST OK；WAV 960044                                                                                                                                                                                                                                     |
| Kotlin (Swing)               | `08_kotlin`       | `dist/kotlin/XU808-Kotlin.jar`、app-image EXE                                 | 自检 16/16 声部、JSON 往返、MIDI 268 字节、WAV 960044；与 Java 峰值一致                                                                                                                                                                                                                    |
| PHP（MVC 网页版）                 | `14_php`          | `14_php/public` + `index.php`                                                | PHP 内置服务器 HTTP 200；服务端导出 MIDI（MThd，268 字节）                                                                                                                                                                                                                                |
| VB.NET (WinForms)            | `11_vbnet`        | `dist/vbnet/XU808-VBNet.exe`（50 MB，单文件）                                      | `--selftest` exit 0；WAV 960044、MIDI 268、工程 3.5 MB                                                                                                                                                                                                                         |
| Go (Win32+waveOut)           | `12_go`           | `dist/go/XU808-Go.exe`（2.6 MB）                                               | 独立目录连续 `--selftest` exit 0；WAV 960044、Song WAV、MIDI 268、JSON 2.5 MB；已修复 Windows 覆盖重命名问题                                                                                                                                                                                   |
| JSP（MVC 网页版）                 | `09_jsp`          | `dist/xu808-jsp.war`                                                         | Tomcat 9 部署，首页 200；`/api/project` 2.19 MB；WAV 960044 / Song 3648044；MIDI MThd 268；见 `VERIFY.txt`                                                                                                                                                                          |
| JavaEE（MVC 网页版）              | `10_javaee`       | `dist/xu808-javaee.war`                                                      | 同上，独立 WAR，首页 200；WAV/MIDI 头正确                                                                                                                                                                                                                                             |
| Rust (Win32, 零 crate)        | `13_rust`         | `dist/rust/XU808-Rust.exe`（694 KB，WINDOWS_GUI）                               | `--selftest` exit 0；WAV 960044、MIDI 268、JSON 332 KB；GUI 实测：每轨长度、歌曲链 repeats、LP/HP 开关、播放头/电平表                                                                                                                                                                              |
| Ruby (Fiddle + Win32 GDI)    | `16_ruby`         | `16_ruby/run.bat` + `lib/xu808/*` + `tools/ruby` 便携运行时（36.3 MB）              | `run.bat --selftest` → SELFTEST OK：16 声部峰值 0.24–0.43，WAV 960044（240000 帧/480000 样本，peak 0.433），MIDI 382，JSON 往返；`--uismoke` → UISMOKE OK；WAV/MIDI/16 分轨导出、左键编辑、右键属性面板、关窗退出码 0 均实测。**无单文件 EXE**（GitHub/RubyInstaller 不可达，改用 MSYS2 Ruby 4.0.9 目录运行时），详见 `16_ruby/README.md` |
| ASM (x86-64 NASM)            | `15_asm`          | `dist/asm/XU808-ASM.exe`（55,386 字节，控制台 UI，仅依赖 kernel32/user32/gdi32/winmm）   | `--selftest` exit 0，`selftest_asm.wav` 960,044 字节（RIFF/WAVE 16bit/48k/立体声）；峰值 10,343（C 版 10,777）、**钳满样本 0**；waveOut 四缓冲回调实测推进时钟；按键 SPACE/N/P/1-8/W/Q。修复记录见 `15_asm/STATUS.md`                                                                                             |

## 2. 仍在进行 / 未完成

所有 16 个版本均已有可运行产物。ASM 版是**控制台 UI 子集**：16 声部 DSP、16 步音序、播放/停止、
切 Pattern（N/P/1-8）、状态行显示当前步与播放状态、离线导出 WAV 已实现；
网格鼠标编辑、滑杆、电平表、MIDI 收发、工程保存/加载未在汇编版提供，属已知取舍。
原先的“叠加过响 / 削波”已定位并修复（软限幅 `andps` 掩码按 double 写，负半周未取绝对值），详见 `15_asm/STATUS.md`。

> 徐某不会把“没跑过的”写成“已完成”。上表随实际进度更新。

## 3. 统一验证脚本

```powershell
cd xu808_all
powershell -ExecutionPolicy Bypass -File .\verify_all.ps1
```

覆盖：C / Qt / C# / Java / Python / Electron / PHP。
Android 用 `aapt dump badging` + `apksigner verify` 单独验证。

## 4. 已知差异与限制（如实记录）

1. **Java Sound / Electron 延迟**：受系统音频栈限制，击打响应约数十毫秒；
   C / Qt / C# 版本使用 waveOut 四缓冲，延迟约 21–30ms。
2. **Android 未上真机**：无设备/模拟器，仅编译 + JVM DSP 自检；AudioTrack 实际发声需真机确认。
3. **参数平滑**：部分版本（Java/Kotlin）为保持 DSP 公式一致，未加一阶平滑；
   极端拖动参数时可能有轻微 zipper noise。
4. **ASM 版本**：功能范围受汇编工程量限制，未实现项会在 `15_asm/STATUS.md` 明确列出。
5. **单文件打包误报**：PyInstaller / .NET single-file 可能被杀毒软件误判，请加白名单。
6. **Ruby 版本无单文件 EXE、延迟约 64ms**：RubyInstaller/GitHub 不可达，改用
   MSYS2 Ruby 4.0.9 便携目录（`16_ruby/tools/ruby`）+ `run.bat` 一键启动；
   音频 6×512 帧缓冲，密集 16 声部离线渲染约 1.4–1.9 倍实时，
   实时线程偶有 `ovr`（单块 > 32ms），UI 会显示 `ovr/xrun` 计数。
   详见 `16_ruby/README.md` 第 9 节。

## 5. 复现构建

见 `README.md` 第 4 节；七种语言版本见 `docs/README_*.md` 与对应 docx/ppt。
