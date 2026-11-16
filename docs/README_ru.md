# XU-808 — 16-голосовая аналоговая драм-машина · Сюй Моу

> Одна спецификация, семнадцать технологических стеков, один звук.
> Каждый звук синтезируется в реальном времени (осцилляторы + шум + фильтры + огибающие), без сэмплов.

## 1. Введение

XU-808 — программная драм-машина, повторяющая поведение TR-808: 16 голосов, секвенсор на 16/64 шага,
покосэмпловые часы, swing, вероятность, микротайминг, ratchet, акцент, mute/solo, мастер-фильтр и
drive, экспорт WAV/MIDI, сохранение проекта, MIDI-вход и внешние часы. Огибающие — аналитические
функции времени, поэтому каждый удар заканчивается сам, а голос освобождается: звуки не накапливаются.

Одна и та же `00_shared/XU808_SPEC.md` реализована на Python, Java, чистом C, Qt C++, Electron,
Android (Java), C#, Kotlin, JSP, JavaEE, VB.NET, Go, Rust, PHP, x86 ASM и Ruby.

## 2. Модули и вызовы

```
UI-поток ──запись──> данные Project (Pattern/Pad/Master/Song)
    │                     │ только чтение
    │ снимок 30fps        v
    └────────────> Engine (секвенсор + пул голосов)
                          │ рендер по блокам
                          v
                   аудио-поток (waveOut/QAudioSink)
```

- **Model**: Project / Pattern / Step / PadParams / Master / Song — только данные.
- **DSP**: Biquad + Voice; все 16 формул одинаковы во всех языках.
- **Engine**: `samplePos` (int64) — единственные часы; каждое событие переводится в абсолютный
  отсчёт; мастер-цепочка HP → LP → Drive → мягкий клип → громкость.
- **Audio**: отдельный поток высокого приоритета; запрещены выделение памяти, блокировки, файловый
  ввод-вывод и вызовы UI.
- **UI**: редактирует данные и перерисовывается 30 раз в секунду; не управляет секвенсором.

## 3. Возможности

- 16 голосов: BD, SD, LT, MT, HT, LC, MC, HC, RS, CL, CP, MA, CB, CY, OH, CH.
- Для голоса: Tune / Decay / Tone / Snappy / Tail / Level / Pan со сглаживанием.
- 16/64 шагов; на шаг: on, velocity, accent, probability, micro, ratchet, condition.
- Swing, независимая длина дорожки (полиметр), цепочка песен, смена паттерна на границе такта.
- Choke открытого/закрытого хэта, 4 голоса на пэд, voice stealing.
- Мастер HP/LP biquad, drive, мягкий лимитер, индикаторы уровня.
- Экспорт WAV 16 бит/48 кГц, MIDI SMF Format 1, стемы (в отдельных версиях).
- Версионируемый JSON-проект.
- MIDI Note On, CC7/74/71, Program Change, синхронизация по часам.

## 4. Проверенные команды сборки (Windows)

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

### Чистый C (Win32 + waveOut, статически)

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++ (статический Qt 6.8.1)

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C# (.NET WinForms, без NuGet)

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

### PHP (MVC-веб)

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. Проверка нулевых зависимостей

1. Чистая Windows без Python/JDK/.NET/Qt/Node; скопировать только EXE или папку.
2. Запустить; в диспетчере задач не должно быть интерпретатора/рантайма (C, Qt, C#).
3. Пробел/PLAY — звучит стандартный грув 808; 20 ударов по одному бочке не накапливаются.
4. `XU808-*.exe --selftest` создаёт WAV 960044 байт (16 бит/48 кГц, стерео) и `SELFTEST OK`.
5. После закрытия окна не остаётся аудиопотоков/процессов.

## 6. Использование

- Пробел: пуск/стоп; 1–8: паттерн; Z X C V B N M A S D F G H J K L: пэды.
- Левый клик по ячейке: вкл/выкл; правый: velocity/accent/probability/micro/ratchet/condition.
- M/S слева: mute/solo; клик по имени пэда — прослушать.
- Нижние ползунки: мастер LP/резонанс/HP/drive/громкость; индикаторы в реальном времени.
- Проект сохраняется в `.xu808` (JSON); WAV/MIDI — нижними кнопками.

## 7. Правила реального времени

- Часы: int64 `samplePos` — единственный источник; таймеры UI только перерисовывают.
- События: шаг, swing, micro, ratchet переводятся в абсолютные отсчёты.
- Choke: закрытый хэт останавливает открытый с 10 мс фейдом.
- Переиспользование голосов: естественное затухание или stealing; жёсткий предел 4 с.
- Мастер-эффекты имеют отдельное состояние biquad на канал.
- Офлайн-экспорт использует тот же `render()`, что и реальное время.

## 8. FAQ

- **Нет звука**: проверить устройство вывода; Java/Electron требуют разрешения на устройство.
- **Большая задержка**: Java Sound и Electron ограничены аудиостеком ОС.
- **Антивирус**: однофайловые упаковщики часто ложно срабатывают; добавить исключение.
- **Нет MIDI**: подключить устройство и перезапустить; Electron требует разрешение Web MIDI.

## 9. Структура

```
00_shared · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. Авторство

Автор: **Сюй Моу (徐某)**. TR-808 — товарный знак Roland; проект не связан с Roland.
