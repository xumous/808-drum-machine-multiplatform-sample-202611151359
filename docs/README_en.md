# XU-808 16-Voice Analog Drum Machine · by Xu Mou

> One specification, seventeen technology stacks, one sound.
> Every voice is synthesized in real time from oscillators + noise + filters + envelopes. No samples.

## 1. Introduction

XU-808 is a software drum machine modelled on the TR-808 signal behaviour: 16 voices,
16/64-step sequencer, sample-accurate clock, swing, probability, micro-timing, ratchet,
accent, mute/solo, master filter and drive, WAV/MIDI export, project save/load, MIDI input
and external clock. Envelopes are analytic functions of time, so every hit ends by itself and
the voice is released — sounds never pile up.

The same `00_shared/XU808_SPEC.md` is implemented in Python, Java, pure C, Qt C++, Node/Electron,
Android (Java), C#, Kotlin, JSP, JavaEE, VB.NET, Go, Rust, PHP, x86 ASM and Ruby.

## 2. Modules and call flow

```
UI thread ──writes──> Project data (Pattern/Pad/Master/Song)
    │                         │ read-only
    │ 30fps snapshot         v
    └──────────────> Engine (sequencer + voice pool)
                              │ render per block
                              v
                     Audio callback thread (waveOut/QAudioSink)
```

- **Model**: Project / Pattern / Step / PadParams / Master / Song — data only.
- **DSP**: Biquad + Voice; all 16 synthesis formulas identical across languages.
- **Engine**: `samplePos` (int64 samples) is the only clock; every event becomes an absolute
  sample time; master chain is HP → LP → Drive → soft clip → volume.
- **Audio**: dedicated high-priority thread/callback; no heap allocation, locks, file I/O or UI calls.
- **UI**: edits data and refreshes at 30fps; never drives the sequencer.

## 3. Features

- 16 voices: BD, SD, LT, MT, HT, LC, MC, HC, RS, CL, CP, MA, CB, CY, OH, CH.
- Per-voice Tune / Decay / Tone / Snappy / Tail / Level / Pan with smoothing.
- 16/64 steps; per step on, velocity, accent, probability, micro offset, ratchet, condition.
- Swing, per-track length (polymeter), song chain, pattern switching on bar boundaries.
- Open/closed hi-hat choke, 4-voice polyphony per pad, voice stealing.
- Master HP/LP biquad, drive, soft limiter, level meters.
- Export 16-bit/48kHz WAV, SMF Format 1 MIDI, stem WAV (selected versions).
- Versioned JSON project save/load.
- MIDI Note On, CC7/74/71, Program Change, clock sync.

## 4. Verified build commands (Windows)

### Python (PySide6 + PortAudio)

```powershell
cd 01_python_qt
python -m pip install -r requirements.txt
python main.py --selftest
python -m PyInstaller --noconfirm --clean --onefile --windowed --name XU808-Python ^
  --icon app.ico --add-data "app.ico;." ^
  --add-binary "%PYTHON%\Lib\site-packages\_sounddevice_data\portaudio-binaries\libportaudio64bit.dll;_sounddevice_data\portaudio-binaries" ^
  main.py
```

### Java (Swing + javax.sound + jpackage)

```powershell
cd 02_java_swing
javac -encoding UTF-8 -d out src\xu808\*.java
jar --create --file ..\dist\XU808-Java.jar --main-class xu808.XuMain -C out .
jpackage --type app-image --name XU808-Java --input out ^
  --main-jar ..\dist\XU808-Java.jar --main-class xu808.XuMain ^
  --icon app.ico --dest ..\dist\java
```

### Pure C (Win32 + waveOut, static)

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++ (static Qt 6.8.1)

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C# (.NET WinForms, zero NuGet)

```powershell
cd 07_csharp
dotnet publish -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true ^
  -o ..\dist\csharp
```

### Node / Electron (AudioWorklet)

```powershell
cd 05_electron
npm install
npx electron .
```

### PHP (MVC web)

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. Zero-dependency verification

1. Use a clean Windows without Python/JDK/.NET/Qt/Node; copy only the EXE or release folder.
2. Launch it; Task Manager should show no interpreter/runtime process (C, Qt, C#).
3. Press Space/PLAY; the default 808 groove must sound; hit the same kick 20 times — no pile-up.
4. Run `XU808-*.exe --selftest`; it must produce a 960044-byte 16-bit/48k stereo WAV and
   print/write `SELFTEST OK`.
5. Close the window; no audio thread or process may remain.

## 6. Usage

- Space: play/stop; 1–8: select pattern; Z X C V B N M A S D F G H J K L: play pads.
- Left click a cell: toggle; right click: velocity/accent/probability/micro/ratchet/condition.
- M/S on the left: mute/solo; click a pad name to audition.
- Bottom sliders: master LP/resonance/HP/drive/volume; meters update live.
- Save project as `.xu808` JSON; export WAV/MIDI with the bottom buttons.

## 7. Real-time rules

- Clock: int64 `samplePos` is authoritative; UI timers only repaint.
- Events: step, swing, micro and ratchet are converted to absolute sample times.
- Choke: closed hi-hat kills open hi-hat with a 10ms fade.
- Voice recycling: natural decay or stealing deactivates a voice; 4s hard cap.
- Master effects keep separate biquad state per channel.
- Offline export calls the same `render()` as real time.

## 8. FAQ

- **No sound**: check the default output device; Java/Electron need device permission.
- **High latency**: Java Sound and Electron are limited by the OS audio stack.
- **Antivirus warning**: single-file packers are often flagged; whitelist or run from source.
- **No MIDI**: reconnect the device and restart; Electron needs Web MIDI permission.

## 9. Layout

```
00_shared/ spec · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. Credits

Author: **Xu Mou (徐某)**. TR-808 is a trademark of Roland; this project is not affiliated with Roland.
