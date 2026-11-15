# node808 —— 808 合成鼓机（Electron + WebAssembly/AudioWorklet）

与 `c808` / `qt808` / `python808` / `java808` / `android808` 共用同一套
`core808` DSP/音序器/导出核心；本目录只提供 Node/Electron 前端。

```
node808/
├─ package.json            electron 33.4.11 + electron-builder 24.13.3
├─ main.js                  主进程：app:// 自定义协议、文件对话框、落盘
├─ preload.js               contextBridge：仅暴露保存/打开/写文件
├─ src/n808_glue.c          薄胶水层（快照打包 / 工程存取 / 稳定操作码）
├─ app/
│  ├─ index.html            1280x720 逻辑界面 + SONG 链编辑器弹窗
│  ├─ style.css
│  ├─ renderer.js           Canvas 自绘 UI、交互、状态快照解析
│  ├─ audio-worklet.js      ★ 音频线程：wasm 实例 + c808_render()
│  ├─ wasi.js               WASI preview1 → 内存文件系统（导出用）
│  ├─ export-worker.js      Web Worker 离线导出（WAV / 分轨 / MIDI）
│  ├─ engine.js             降级用的本地引擎（无 AudioWorklet 时）
│  └─ wasm/core808.wasm     预编译产物（core808.c + core808_io.c + glue）
├─ tools/
│  ├─ build-wasm.ps1        zig cc -target wasm32-wasi 构建 wasm
│  ├─ selftest.cjs          Node 直接验证 wasm（音序/快照/导出/工程）
│  ├─ test-export.cjs       原样执行 export-worker.js 验证三条导出路径
│  └─ screenshot.ps1        运行截图脚本
└─ dist/node808-portable.exe  便携版 EXE（electron-builder portable）
```

## 1. 架构（DSP 不在渲染进程的事件循环里）

```
UI 线程 (renderer.js, Canvas)
   │  ① 参数：port.postMessage({op,a,b,c,n,f})            （~几十次/秒）
   ▼
AudioWorklet 音频线程 (audio-worklet.js)
   │  ② n808_api() → c808_set_*() → 无锁 SPSC 命令队列（core808.c）
   │  ③ c808_render(engine, L, R, 128)  ← 真正的 DSP，全在 wasm 里
   │  ④ 30fps 把 10856 字节状态快照写进乒乓缓冲区并 transfer 回 UI
   ▼
UI 线程读快照绘制播放头/电平/CPU/XRUN/F 帧计数
```

* **wasm 就是 `core808.c` + `core808_io.c` 本体**，没有一行 JS DSP 参与运算。
  用 zig 自带的 clang 以 wasi reactor 模式编译，`--export` 只暴露需要的符号。
* WASI 的 `fd_write/fd_seek/path_open…` 在浏览器里没有文件系统：
    * AudioWorklet 里全部是空实现（实时路径根本不做文件 I/O）；
    * `export-worker.js` 里由 `wasi.js` 提供内存文件系统，导出完成后把文件字节回传，
      由主进程落盘。这样 `c808_export_wav/c808_export_midi` 一行不改就能工作。
* 音频线程稳态**零分配**：输出缓冲区、命令缓冲区、快照缓冲区都在 init 时
  用 `malloc` 分配一次；`process()` 只做 `c808_render()` + 两次 `TypedArray.set()`。
  快照用两块预分配缓冲乒乓传递，主线程读完再 transfer 回去；音频线程如果拿不到
  空闲缓冲，就直接跳过这一帧快照（绝不阻塞音频）。
* JS 不依赖 `core808` 的命令枚举编号（C 枚举的值与前端硬编码极易错位），
  统一走 `src/n808_glue.c` 里稳定的 `n808_api(engine, op, ...)` 操作码。

### 快照布局（`n808_snap`，10856 字节）

|    偏移 | 类型           | 内容                                                                  |
|------:|--------------|---------------------------------------------------------------------|
|     0 | int32[8]     | playing, pattern, step, songPos, loopCount, xruns, frames, blocks   |
|    32 | float        | CPU 负载 0..1                                                         |
|    36 | float[18]    | 16 轨电平 + masterL + masterR                                          |
|   108 | int32[8]     | len, swing, curPattern, songOn, patternCount, tsNum, tsDen, songLen |
|   140 | float[103]   | bpm, master[6], track[16][6]                                        |
|   552 | uint8[10240] | 步进数据 16×64×10（on/vel/accent/prob/ratchet/rate/cond/condN/micro）     |
| 10792 | uint8[64]    | mute / solo / trackLen / midiNote                                   |

## 2. 功能

* 传输：PLAY/STOP、BPM 20–400（点击框拖拽或滚轮）、TAP 测速、SWING。
* Pattern：选择/长度 1–64/复制到下一个/CLEAR、16 个快捷槽 + 16 页（共 256）。
* 网格：16 轨 × 长度，点击开关；`Ctrl` 重音、`Shift` 概率循环、`Alt` 连击循环、
  滚轮力度；右侧面板编辑力度/概率/连击/间距/微移/条件/重音。
* 声部：6 个旋钮（TUNE/DECAY/TONE/CHAR/LEVEL/PAN）+ TRIGGER。
* 主控：VOLUME/DRIVE/HP/LP/RESO/ACCENT（饱和 + 滤波 + 重音量）。
* 静音/独奏、16 个打击垫、播放头、每轨电平表、状态栏 CPU/XRUN/帧计数。
* SONG 链编辑器（增删/排序/换 pattern/重复次数）。
* 工程 `.808` 存取（与其它前端完全相同的文件格式）。
* 导出：WAV、16 轨分轨 WAV、MIDI（全部在 Worker 里离线渲染，不占用音频线程）。
* MIDI 输入：Web MIDI，点击 MIDI 框循环切换设备（含 MIDI 时钟/开始/停止）。
* 快捷键：空格 播放/停止、←/→ 切 pattern、T 测速、`1234 qwer asdf zxcv` 触发打击垫。

## 3. 构建

```powershell
cd node808
npm install                 # electron / electron-builder（本机已缓存，可离线安装）
npm run build:wasm          # 用 zig 重新生成 app/wasm/core808.wasm（已预编译，可跳过）
npm start                   # 开发运行
npm run dist                # 产出 dist/node808-portable.exe
```

`build:wasm` 会依次查找：`-Zig <path>` 参数 → 环境变量 `ZIG` →
`.tools\zig\zig.exe` → `%TEMP%\zigdist\x\*\zig.exe` → PATH 里的 `zig`。
如果没有 Zig，直接跳过即可，仓库里已经带了一份编译好的 wasm。

打包用的是 electron-builder 的 **portable** target（单文件 EXE，双击即用，
无需安装；`app.ico` 取自仓库根目录）。

## 4. 降级与已知限制

* **没有音频设备 / AudioWorklet 起不来**：同一个 wasm 会在渲染进程里以 25ms
  定时器静默推进（状态栏显示 `NO AUDIO DEVICE · LOCAL ENGINE (SILENT)`），
  编辑、工程存取、导出全部照常可用，只是没有声音。若 8 秒内既没有渲染回调
  也没有快照，看门狗会自动切到该模式。
* **AudioWorkletGlobalScope 没有 `performance`**（Chromium 只在 Window/Worker 暴露），
  CPU 统计退化到 `Date.now()` 的 1ms 精度（按 32 块 ≈85ms 累计），仅影响显示。
* 声像为等功率 Law：PAN=50%（居中）时左右各 0.7071，PAN=0 全左、PAN=100 全右。
  （早期版本把相位按“圈”误算成 0.25 圈，导致居中时左声道为 0；现已在共享核心
  `core808.c` 修正为 `a = (pan+1) * 0.125`，并用 `core808/test_core.exe` 的
  “center pan produces signal on both L and R” 回归项锁定。）
* Web MIDI 需要系统里有 MIDI 输入设备；没有时状态栏显示 `MIDI: NONE`。
* 便携版 EXE 首次启动会自解压到临时目录，约 2–3 秒后才出现窗口。

## 5. 验证

```powershell
node tools\\selftest.cjs       # wasm：音序触发 / 快照 / WAV+分轨+MIDI / 工程存取
node tools\\test-export.cjs    # 原样跑 export-worker.js 的三条导出路径
powershell -File tools\\screenshot.ps1   # 启动并截图
```

实测（便携版 EXE）：状态栏 `AUDIOWORKLET 48000 Hz · 2ch · wasm · CPU 1% · XRUN 0 ·
AUDIO CALLBACK OK · STOP · F196352 · BLK1534`，6 秒后变成 `F489216 · BLK3822`
（+2288 块 ≈ 6s × 375 块/s），说明音频线程的 `process()` 回调持续在跑。
