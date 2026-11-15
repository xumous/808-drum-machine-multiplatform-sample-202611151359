# Android 808 鼓机（android808）

基于共享 C 核心 `core808` 的原生 Android 版 808 模拟鼓机。完整复刻桌面版
`java808` / `qt808` 的功能与深橙色 808 视觉，并针对触摸屏重新设计布局。

## 功能

- **16 声部模拟合成引擎**：KICK / SNARE / CLAP / RIM / TOM LO-MID-HI / CONGA /
  COWBELL / SHAKER / TAMB / CRASH / RIDE / HAT OPEN / HAT CLOSED / CLAVE。
  每个声部 6 个参数旋钮：TUNE / DECAY / TONE / CHAR / LEVEL / PAN（垂直拖动）。
- **主控**：VOLUME / DRIVE / HP / LP / RESO / ACCENT 六个旋钮（垂直拖动）。
- **16 轨 × 16/32/64 步音序器**：点击切换开关，长按切换重音（ACCENT）。
  64 步时分 4 页显示（每页 16 列），播放时自动跟随播放头，也可用 `< >` 手动翻页；
  点击页码 `1/4` 可切换为 ALL 模式，一屏显示全部 64 列。
- **每步细节**：VELOCITY / PROB / RATCHET / RATE / MICRO 五个滑条，
  COND（ALWAYS、1:2、1:3、1:4、FILL、NOT FILL、EVERY N）与 N 值按钮。
- **轨道控制**：MUTE / SOLO / 电平表 / 轨道名点选 / 16 个打击垫（即点即响）。
- **走带**：PLAY / STOP / TAP TEMPO / BPM 拖动 / SWING 拖动 / 16-64 长度切换，
  以及拍号按钮 `TS 4/4`（循环 4/4、3/4、5/4、7/8、6/8、12/8）和
  `SYNC INT` / `SYNC MIDI` 时钟源切换按钮。
- **Pattern**：256 个 pattern 槽位，底部分页快速选择、COPY>（复制到下一个）、
  长按 CLEAR 清除当前 pattern。
- **Song 模式**：SONG 开关 + EDIT SONG 覆盖层，支持添加/删除/上下移动/
  修改 pattern 与重复次数，DONE 应用。
- **保存/加载**：工程写入 APP 专属外部目录，LOAD 覆盖层列出 `.808` 工程，
  点选加载（后台线程执行，不阻塞 UI）。
- **导出**：WAV（pattern/song，2 循环 + 2 秒尾音）、MIDI（SMF0，PPQ960，
  鼓通道 10）、STEMS（16 个分轨 WAV）。全部在 worker 线程完成。
- **MIDI 输入**：通过 `android.media.midi` 接收 USB MIDI 音符、Clock、
  Start/Stop/Continue、Program Change，并推入 core808 的无锁 MIDI 队列。
  第二行的 `MIDI: ...` 按钮用于打开/关闭 MIDI 输入。
- **时钟源**：`SYNC INT` 使用内部时钟；`SYNC MIDI` 时音序器只跟随外部
  MIDI Clock（需连接 MIDI 设备并开启 MIDI 输入）。
- **实时显示**：播放头、16 路电平表、CPU 负载、XRUN 计数，Choreographer
  驱动（约 60 fps，远高于 30 fps 要求）。

## 架构

```
UI 线程 (MachineView, Canvas 自绘)
   │  命令队列（无锁）：setStep/setTrackParam/setMaster/...
   ▼
core808.c（C99，DSP + 采样级音序器）
   ▲
   │  c808_render() / c808_render_interleaved()
AudioEngine808 专用线程（AudioTrack，48 kHz 立体声，优先 float32 + LOW_LATENCY）
```

- 音频只在 `AudioEngine808` 的 `808-audio` 线程中渲染，线程优先级
  `THREAD_PRIORITY_URGENT_AUDIO`；UI 线程从不调用渲染。
- 渲染块 256 帧，AudioTrack 使用 `PERFORMANCE_MODE_LOW_LATENCY`；
  float32 打开失败时自动回退 PCM16。
- 所有 UI→音频参数都通过 core808 的无锁环形命令队列传递。
- UI 每帧通过 `c808_snapshot()` / `c808_get_status()` 读取一次性快照与
  原子状态，不触碰音频线程私有数据。
- JNI 桥：`app/src/main/jni/jni808_android.c`；CMake 目标 `808core`
  → `lib808core.so`。
- 离线导出在工作线程创建独立引擎，复用同一 `c808_render()` 渲染路径。

## 目录

```
android808/
├── app/src/main/java/com/guji808/android/
│   ├── MainActivity.java      生命周期 / 沉浸式全屏 / 音频焦点
│   ├── MachineView.java       全部 Canvas UI、触摸、Song/文件覆盖层
│   ├── AudioEngine808.java    AudioTrack 专用高优先级音频线程
│   ├── Native808.java         JNI 声明
│   └── MidiInput.java         android.media.midi 输入
├── app/src/main/jni/
│   ├── core808.c / core808.h / core808_io.c   （复制自 ../core808）
│   ├── jni808_android.c       JNI 桥
│   └── CMakeLists.txt
├── app/src/main/res/          图标（由 ../app.ico 用 ffmpeg 生成）、主题
├── keystore/android808-release.jks
├── dist/app-debug.apk
├── dist/app-release.apk
└── README.md
```

## 构建

要求：JDK 17、Android SDK（platform android-33、build-tools 30.0.3+）、
NDK `27.0.12077973`、CMake `3.22.1`。`local.properties` 中的 `sdk.dir`
请按本机 SDK 位置修改。

```bat
set JAVA_HOME=<jdk-17 路径>
gradlew.bat assembleDebug assembleRelease
```

本项目使用 AGP 7.4.2 + Gradle 7.5（已针对 compileSdk 33 / minSdk 28 /
targetSdk 33 验证）。Gradle 依赖与 NDK/CMake 均可离线复用缓存。

## 签名

- Keystore：`android808/keystore/android808-release.jks`（JKS）
- Alias：`android808`
- Keystore 密码：`808808`
- Key 密码：`808808`
- 有效期：**365000 天**（约 1000 年）
- DN：`CN=Android808, OU=Audio, O=Guji808, L=Shanghai, ST=Shanghai, C=CN`
- 证书 SHA-256：
  `4a84f44bb7ef9b50b409fb70dae306b695ea7b1001785450e0d492262c496276`
- 最终发布 APK 使用 **APK Signature Scheme v2**（minSdk 28 下已全覆盖）。
  Gradle 内置签名即产出 v2；未再追加 v1（JAR 签名），以避免 Janus 类降级攻击面。
  如需 v1，可在 `assembleRelease` 后用 apksigner 追加：

```bat
"%LOCALAPPDATA%\Android\Sdk\build-tools\36.1.0\apksigner.bat" sign ^
  --ks keystore\android808-release.jks --ks-pass pass:808808 ^
  --key-pass pass:808808 --ks-key-alias android808 ^
  --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled false ^
  --min-sdk-version 23 ^
  --out dist\app-release.apk app\build\outputs\apk\release\app-release.apk
```

> 该 keystore 仅用于本项目自测/分发示例，请勿用于公开商店发布。

## 触摸操作

| 操作                      | 效果                                                      |
|-------------------------|---------------------------------------------------------|
| 点击步进格                   | 开/关该步                                                   |
| 长按步进格                   | 切换该步 ACCENT                                             |
| 拖动旋钮（上下）                | 调整声部/主控参数                                               |
| 拖动 BPM 显示               | 调整 BPM                                                  |
| 点击/拖动 SWING 条           | 调整摇摆量                                                   |
| 点击轨道名                   | 选中声部                                                    |
| 点击 M / S                | 静音 / 独奏                                                 |
| 点击打击垫                   | 触发该声部并选中                                                |
| 点击 pattern 数字           | 切换 pattern                                              |
| 长按 CLEAR                | 清除当前 pattern                                            |
| 点击 COND / N             | 切换条件类型 / EVERY N 的 N                                    |
| 点击 TS 4/4               | 循环拍号（4/4、3/4、5/4、7/8、6/8、12/8）                          |
| 点击 SYNC INT / SYNC MIDI | 切换内部 / 外部 MIDI 时钟源                                      |
| 点击 MIDI: ...            | 打开 / 关闭 MIDI 输入设备                                       |
| EDIT SONG               | 打开 Song 链编辑器（可拖动列表滚动）                                   |
| SAVE / LOAD             | 保存工程 / 打开工程列表                                           |
| WAV / MIDI / STEMS      | 后台导出到 `Android/data/com.guji808.android/files/exports/` |

## 文件位置（Android 11+ 无需存储权限）

- 工程：`Android/data/com.guji808.android/files/projects/`
- 导出：`Android/data/com.guji808.android/files/exports/`

## 验证记录

- `gradlew.bat assembleDebug assembleRelease`：BUILD SUCCESSFUL
- `apksigner verify --print-certs`：Verifies；v2 = true；RSA 2048
  证书有效期 2026-11-15 → 3026-03-18
- `zipalign -c -v 4`：Verification successful
- `aapt dump badging`：`com.guji808.android`，versionCode 1，minSdk 28，
  targetSdk 33，native-code `arm64-v8a armeabi-v7a x86_64`
- APK：
    - `dist/app-debug.apk`（464,565 字节，SHA-256
      `371384ebacafe37bc23d5dbf5fd801d6af61a9e15605a910808fc8426d980b3a`）
    - `dist/app-release.apk`（384,193 字节，SHA-256
      `263873dbbb024a68288594a07db95d3cc49d72d66826602d12edb088c2c4b837`）
- JNI 导出符号已用 `llvm-nm -D` 核对：`Java_com_guji808_android_Native808_*`
  （包括 renderF32 / renderS16 / snapshot / status / exportWav / exportMidi /
  pushMidi / setClockSource / getClockSource 等）

## 已知限制 / 说明

- 本机 `sdkmanager` 安装 NDK 时在解压阶段异常退出，因此 NDK
  `27.0.12077973` 采用官方 `android-ndk-r27-windows.zip` 手工解压到
  `%LOCALAPPDATA%\Android\Sdk\ndk\27.0.12077973`（`source.properties`
  完整，Gradle/CMake 识别正常），不影响构建。
- 64 步 pattern 默认按 16 列分页显示（触摸可点性优先），点击页码可切换
  ALL 模式显示完整 64 列；音序长度仍为完整 16/32/64 步。
- 运行期音频试听无法在当前构建环境完成：本机 x86_64 模拟器缺少硬件虚拟化
  （VT-x/AMD-V），AVD 启动被拒绝；已通过 JNI/CMake 编译、APK 打包与符号核对
  验证音频线程代码路径存在。建议在真机上试听。
- MIDI 输入使用 USB/虚拟 MIDI 设备；未申请蓝牙 MIDI 权限（需要
  `BLUETOOTH_CONNECT`，可按需补充）。
- 导出分轨（STEMS）会在目标 WAV 同目录生成 16 个 `*_kick.wav` 等文件。
