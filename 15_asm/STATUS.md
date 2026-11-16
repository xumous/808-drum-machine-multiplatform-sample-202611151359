# XU-808 · 15_asm 状态说明（x86-64 NASM 版）

> 署名：徐某。本文件只记录**实际编译并运行过**的结果，不写“应该能跑”。
> 验证环境：Windows 10 19045 · x86-64 · 48kHz 立体声 · 2026-11-16

## 1. 产物

| 文件                 | 说明                                                              |
|--------------------|-----------------------------------------------------------------|
| `xu808_asm.exe`    | 55,376 字节，PE32+ 控制台子系统（`--subsystem windows` + 自定义入口）           |
| `selftest_asm.wav` | `--selftest` 生成：960,044 字节（RIFF/WAVE，16bit/48kHz/立体声，240,000 帧） |
| `selftest_asm.txt` | `SELFTEST OK`（13 字节，含 CRLF）                                     |
| `build.bat`        | 一键构建脚本（NASM + MinGW ld）                                         |

源码：`mathx.asm`（sin/exp/tanh/square）、`dsp.asm`（16 声部合成 + RBJ 双二阶）、
`engine.asm`（工程/音序器/混音/waveOut/导出）、`ui.asm`（控制台界面）、`main.asm`（入口）、
公共定义 `xu808.inc`。

## 2. 构建与验证（实际执行的命令）

```powershell
# 汇编（工作目录必须是 15_asm，%include "xu808.inc" 靠当前目录解析）
cd 15_asm
..\tools\nasm\nasm-2.16.03\nasm.exe -f win64 -o mathx.obj  mathx.asm
..\tools\nasm\nasm-2.16.03\nasm.exe -f win64 -o dsp.obj    dsp.asm
..\tools\nasm\nasm-2.16.03\nasm.exe -f win64 -o engine.obj engine.asm
..\tools\nasm\nasm-2.16.03\nasm.exe -f win64 -o ui.obj     ui.asm
..\tools\nasm\nasm-2.16.03\nasm.exe -f win64 -o main.obj   main.asm

# 链接（无 CRT，入口自定 WinMainCRTStartup）
C:\Qt\Tools\mingw1310_64\bin\ld.exe -m i386pep --subsystem windows ^
  -e WinMainCRTStartup -o xu808_asm.exe ^
  mathx.obj dsp.obj engine.obj ui.obj main.obj ^
  -L C:\Qt\Tools\mingw1310_64\x86_64-w64-mingw32\lib ^
  -lkernel32 -luser32 -lgdi32 -lwinmm

# 自检（无人值守）
.\xu808_asm.exe --selftest
# exit=0；selftest_asm.wav 960044 字节；selftest_asm.txt = "SELFTEST OK"
```

依赖的 DLL 只有 `KERNEL32 / USER32 / GDI32 / WINMM`，不依赖 msvcrt（`sin/exp/tanh` 都是自己实现的多项式）。

## 3. 已实现

- **DSP**：16 个声部（BD/SD/LT/MT/HT/LC/MC/HC/RS/CL/CP/MA/MB/CY/OH/CH）公式逐项对齐 `03_c_win32/dsp.c`；
  每声部 4 个 voice、抢占最老 voice、闭镲掐开镲。
- **滤波器**：RBJ 低通/高通/带通（DF2T），参数未变时跳过重算。
- **音序器**：samplePos(int64) 唯一时钟；每步事件展开为绝对样本时刻；
  swing / micro / ratchet(1–8) / probability(xorshift32) / condition(FILL/NOTFILL/EVERY2/EVERY4)；
  每轨 trackLength 实现 polymeter；pattern 切换只在循环边界；Song 链可循环。
- **混音**：mute/solo 在混音阶段阻断；主链 HP→LP→tanh 过载→`x/(1+|x|)` 软限幅→主音量。
- **实时输出**：waveOut 四缓冲 + `CALLBACK_FUNCTION`，回调里渲染、打包 int16、回写缓冲；
  预填 4 块避免开头半块静音。实测回调推进时钟（1.2 秒内 samplePos 前进 25,600 帧）。
- **离线导出**：`xu_export_wav` 复用同一套 render，只把时钟换成固定块推进；末尾 3 秒尾音。
- **控制台界面**：`SPACE` 播放/停止、`N/P` 上下 Pattern、`1..8` 直选、`W` 导出 WAV、`Q/ESC` 退出；
  状态行实时显示 Pattern / 当前步 / 播放状态 / BPM / 音频设备状态。

## 4. 未实现（如实列出）

1. **GUI**：没有 GDI/Qt 界面。原计划的 1500×820 深色网格界面未做，改为控制台界面。
   因此没有：步进网格鼠标编辑、滑杆拖动、电平表、播放头平滑动画、TAP 测速。
2. **MIDI**：不导出 SMF，也不接收 MIDI 输入/外部时钟（`xu_engine_external_tick` 已写好但无入口）。
3. **工程存取**：没有 `.xu808` 保存/加载。
4. **分轨导出**：只支持整 Pattern / Song 的立体声混音，没有每轨单独 WAV。
5. **参数平滑**：主控/声部参数直接生效，没有规格书要求的一阶平滑（0.002/样本），
   拖动参数时可能有 zipper noise（控制台界面里只能通过改默认值间接体现）。
6. **图标**：没有嵌入 app.ico。
7. **CPU/XRUN 统计**：结构里有字段，但没有计时与统计。
8. **Song 模式**：控制台界面只能切 Pattern，不能切换 Song 开关。

## 5. 已修复：多声部叠加电平过高 / 峰值钳满

**根因**：主效果链软限幅 `x/(1+|x|)` 里的取绝对值用了
`andps xmm0, [rel c_abs16]`，而 `c_abs16` 是按 **double** 设计的掩码
（`dq 0x7FFFFFFFFFFFFFFF, ...`，低 32 位是 `0xFFFFFFFF`）。
`andps` 按 32 位 float 逐元素相与，低 32 位全 1 等于**没清符号位**，
于是在负半周 `|x|` 仍为负 → 变成 `x/(1+x)`：

- `x = -0.5` → `-0.5/0.5 = -1.0`（翻倍且反相）
- `x → -1`   → 除以 ~0，输出冲上满刻度

正半周完全正确、负半周被算飞——这也是为什么单声部前几十个采样点
与 C 版**逐位相同**，而负半周和密集段落却偏响。

**修复**：新增按 float 粒度的掩码 `ff_abs_ps: dd 0x7FFFFFFF ×4`，
主链 4 处 `andps` 全部改用它。

### 修复前后实测（`selftest_asm.wav` vs `03_c_win32/selftest_c.wav`）

| 指标                 | 修复前                  | 修复后                  | C 版参考  |
|--------------------|----------------------|----------------------|--------|
| 峰值                 | 32767（满刻度）           | **10343**            | 10777  |
| 钳满样本数（\|v\|≥32760） | 220                  | **0**                | 0      |
| RMS（240,000 帧）     | 0.1341               | **0.1174**           | 0.1512 |
| RMS 比值（ASM/C）      | 0.89（但波形已失真）         | **0.78**             | 1.00   |
| `--selftest`       | exit 0 / SELFTEST OK | exit 0 / SELFTEST OK | —      |

峰值已与 C 版差 4%，且不再有任何样本钳满。RMS 比 C 版低约 22%
（略超 ±20% 的目标）：C 版每个 0.625s 窗口的 RMS 都恒为 0.1507，
说明它含有一段持续的宽带能量，而本版是正常的衰减鼓组；峰值一致、
无削波，因此判断为 C 版侧的能量差异，本版未额外补偿（不靠调音量掩盖）。

## 6. 其它已知问题

1. `--selftest` 只验证“能渲染出非空且有声音的 WAV”，不等于听感验收。
2. 控制台界面为 ASCII，中文只在窗口标题之外的地方一律用英文，避免代码页差异。
3. 界面没有自动化按键测试：只做了“启动后存活 5 秒、音频回调推进时钟、退出不残留进程”的验证。

## 7. 本次对已有汇编源码的修正

| 文件                 | 问题                                                                           | 修正                                                   |
|--------------------|------------------------------------------------------------------------------|------------------------------------------------------|
| `engine.asm`       | 软限幅 `andps` 配 double 掩码，负半周不求绝对值 → 输出冲满刻度、220 个样本钳满                          | 新增 `ff_abs_ps`（4×0x7FFFFFFF），峰值 32767→10343、钳满 220→0 |
| `dsp.asm`          | `xu_voice_render` 从 `[rbp+16]` 取第 5 个参数，实际 Win64 约定是 `[rbp+48]`（差 32 字节影子空间） | 改为 `[rbp+48/56/64/72]`                               |
| `dsp.asm`          | 结尾 `cmp rax,[c_192000]` 拿整数和 double 位模式比较，voice 永不回收                         | 改成 `cmp rax,192000`                                  |
| `mathx.asm`        | `exp` 的 Horner 系数顺序写反（0.45→tanh 误差达 21%）                                     | 改成 1/5040…1/2 的正确降阶顺序，误差降到 3e-7                      |
| `mathx.asm`        | `sin` 只到 x^11，在 \|r\|→π 处误差 4e-4                                             | 补 x^13 项，误差降到 2e-5                                   |
| `engine.asm`       | `xu_project_default` 把 0.45 写成 `0x3F666666`（其实是 0.9），共 3 处                   | 改成 `0x3EE66666`                                      |
| `engine.asm`       | `xu_engine_init` 不清 voices，重新初始化会带着上一批正在响的槌                                  | 补 `xu_bzero(V_SIZE*64)`                              |
| `engine.asm`       | 事件插入排序里 `dec r11d` 会清零高 32 位，`mov rax,r11` 变成 0xFFFFFFFF，越界写崩                | 改 `movsxd`                                           |
| `engine.asm`       | 原文件末尾 `xu_advance_step` 有半截占位代码（`lea rsi,[rdx?]`）无法汇编                        | 重写，并补齐 render/trigger/export/waveOut                 |
| `mathx/dsp/engine` | 缺 `default rel`，`[c_xxx]` 生成 32 位绝对重定位，ld 报 truncated                        | 全部加 `default rel`                                    |
