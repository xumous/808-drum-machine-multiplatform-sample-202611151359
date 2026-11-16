# XU-808 16-Stimmen-Analog-Drummachine · von Xu Mou

> Eine Spezifikation, siebzehn Technologie-Stacks, ein Klang.
> Jeder Klang wird in Echtzeit aus Oszillatoren + Rauschen + Filtern + Hüllkurven synthetisiert – ohne Samples.

## 1. Einführung

XU-808 ist eine Software-Drummachine nach dem Verhalten der TR-808: 16 Stimmen,
16/64-Schritt-Sequenzer, samplegenaue Uhr, Swing, Wahrscheinlichkeit, Micro-Timing, Ratchet,
Accent, Mute/Solo, Master-Filter und Drive, WAV/MIDI-Export, Projekt speichern/laden,
MIDI-Eingang und externe Clock. Hüllkurven sind analytische Zeitfunktionen; jeder Schlag endet
von selbst und die Stimme wird freigegeben – Klänge stapeln sich nie.

Dieselbe `00_shared/XU808_SPEC.md` ist in Python, Java, reinem C, Qt C++, Electron, Android (Java),
C#, Kotlin, JSP, JavaEE, VB.NET, Go, Rust, PHP, x86-ASM und Ruby umgesetzt.

## 2. Module und Aufrufe

```
UI-Thread ──schreibt──> Project-Daten (Pattern/Pad/Master/Song)
    │                        │ nur lesen
    │ 30fps-Snapshot         v
    └──────────────> Engine (Sequenzer + Voice-Pool)
                             │ Render pro Block
                             v
                    Audio-Callback-Thread (waveOut/QAudioSink)
```

- **Model**: Project / Pattern / Step / PadParams / Master / Song – reine Daten.
- **DSP**: Biquad + Voice; alle 16 Syntheseformeln sind in jeder Sprache identisch.
- **Engine**: `samplePos` (int64) ist die einzige Uhr; jedes Event wird absolute Sample-Zeit;
  Master-Kette HP → LP → Drive → Soft-Clip → Lautstärke.
- **Audio**: eigener Thread hoher Priorität; keine Heap-Allokation, Locks, Datei-I/O oder UI-Aufrufe.
- **UI**: bearbeitet Daten und zeichnet mit 30fps; treibt nie den Sequenzer.

## 3. Funktionen

- 16 Stimmen: BD, SD, LT, MT, HT, LC, MC, HC, RS, CL, CP, MA, CB, CY, OH, CH.
- Pro Stimme Tune / Decay / Tone / Snappy / Tail / Level / Pan, geglättet.
- 16/64 Schritte; pro Schritt on, Velocity, Accent, Probability, Micro, Ratchet, Condition.
- Swing, Länge pro Spur (Polymeter), Song-Kette, Pattern-Wechsel an der Taktgrenze.
- Open/Closed-Hi-Hat-Choke, 4 Stimmen pro Pad, Voice Stealing.
- Master-HP/LP-Biquad, Drive, Soft-Limiter, Pegelanzeige.
- Export 16bit/48kHz WAV, SMF Format 1 MIDI, Stem-WAV (ausgewählte Versionen).
- Versioniertes JSON-Projekt.
- MIDI Note On, CC7/74/71, Program Change, Clock-Sync.

## 4. Verifizierte Build-Befehle (Windows)

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

### Reines C (Win32 + waveOut, statisch)

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++ (statisches Qt 6.8.1)

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C# (.NET WinForms, ohne NuGet)

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

### PHP (MVC-Web)

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. Zero-Dependency-Prüfung

1. Sauberes Windows ohne Python/JDK/.NET/Qt/Node verwenden; nur EXE bzw. Ordner kopieren.
2. Starten; im Task-Manager darf kein Interpreter/Runtime erscheinen (C, Qt, C#).
3. Leertaste/PLAY: der Standard-808-Groove muss klingen; 20-mal dieselbe Kick ohne Aufstapeln.
4. `XU808-*.exe --selftest` erzeugt eine 960044-Byte-WAV (16bit/48k Stereo) und `SELFTEST OK`.
5. Fenster schließen; kein Audio-Thread/Prozess darf bleiben.

## 6. Bedienung

- Leertaste: Start/Stop; 1–8: Pattern; Z X C V B N M A S D F G H J K L: Pads.
- Linksklick Zelle: an/aus; Rechtsklick: Velocity/Accent/Probability/Micro/Ratchet/Condition.
- M/S links: Mute/Solo; Pad-Name klicken zum Anhören.
- Regler unten: Master-LP/Resonanz/HP/Drive/Volume; Pegel live.
- Projekt als `.xu808` (JSON) speichern; WAV/MIDI über die Tasten unten.

## 7. Echtzeit-Regeln

- Uhr: int64 `samplePos` ist maßgeblich; UI-Timer zeichnen nur.
- Events: Schritt, Swing, Micro, Ratchet werden in absolute Sample-Zeiten umgerechnet.
- Choke: geschlossene Hi-Hat stoppt offene mit 10ms-Fade.
- Voice-Recycling: natürliches Ende oder Stealing; 4s-Hartgrenze.
- Master-Effekte mit getrenntem Biquad-Zustand pro Kanal.
- Offline-Export nutzt dasselbe `render()` wie Echtzeit.

## 8. FAQ

- **Kein Ton**: Standard-Ausgabe prüfen; Java/Electron brauchen Geräteberechtigung.
- **Hohe Latenz**: Java Sound und Electron sind durch den OS-Audiostack begrenzt.
- **Virenwarnung**: Single-File-Packer werden oft falsch erkannt; Ausnahme hinzufügen.
- **Kein MIDI**: Gerät neu anschließen und neu starten; Electron braucht Web-MIDI-Rechte.

## 9. Verzeichnisse

```
00_shared · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. Credits

Autor: **Xu Mou (徐某)**. TR-808 ist eine Marke von Roland; dieses Projekt ist nicht mit Roland verbunden.
