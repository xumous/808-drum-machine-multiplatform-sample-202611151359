# XU-808 16ボイス・アナログ・ドラムマシン · 徐某

> ひとつの仕様、17 の技術スタック、ひとつの音。
> すべての音はオシレーター + ノイズ + フィルター + エンベロープでリアルタイム合成され、サンプルは使いません。

## 1. 概要

XU-808 は TR-808 の回路挙動を参考に徐某が実装したソフトウェア・ドラムマシンです。
16 ボイス、16/64 ステップ・シーケンサー、サンプル精度のクロック、スウィング、確率、
マイクロタイミング、ラチェット、アクセント、ミュート/ソロ、マスター・フィルター、
ドライブ、WAV/MIDI 書き出し、プロジェクト保存、MIDI 入力を備えます。
エンベロープは時間の解析関数なので、各ヒットは自然に終了しボイスは解放されます。
音が積み重なることはありません。

同じ `00_shared/XU808_SPEC.md` を Python、Java、純 C、Qt C++、Electron、Android(Java)、
C#、Kotlin、JSP、JavaEE、VB.NET、Go、Rust、PHP、x86 ASM、Ruby で実装しています。

## 2. モジュールと呼び出し関係

```
UI スレッド ──書込──> Project データ (Pattern/Pad/Master/Song)
    │                       │ 読取専用
    │ 30fps スナップショット v
    └──────────────> Engine（シーケンサー + ボイスプール）
                            │ ブロックごとに描画
                            v
                    オーディオ・コールバック（waveOut/QAudioSink）
```

- **Model**：Project / Pattern / Step / PadParams / Master / Song。データのみ。
- **DSP**：Biquad + Voice。16 の合成式は全言語で同一。
- **Engine**：`samplePos`（int64 サンプル）が唯一のクロック。全イベントを絶対サンプル時刻に変換。
  マスター順序は HP → LP → Drive → ソフトクリップ → 音量。
- **Audio**：専用高優先度スレッド。ヒープ確保・ロック・ファイル IO・UI 呼び出し禁止。
- **UI**：データ編集と 30fps 再描画のみ。シーケンサーを駆動しない。

## 3. 機能

- 16 ボイス：BD、SD、LT、MT、HT、LC、MC、HC、RS、CL、CP、MA、CB、CY、OH、CH。
- 各ボイスに Tune / Decay / Tone / Snappy / Tail / Level / Pan（平滑化）。
- 16/64 ステップ。各ステップに on、ベロシティ、アクセント、確率、マイクロ、ラチェット、条件。
- スウィング、トラック別長さ（ポリメーター）、ソングチェーン、小節境界でのパターン切替。
- オープン/クローズド・ハイハットのチョーク、4 ボイス・ポリフォニー、ボイス・スティーリング。
- マスター HP/LP、ドライブ、ソフトリミッター、レベルメーター。
- 16bit/48kHz WAV、SMF Format 1 MIDI、ステム WAV（一部）。
- バージョン付き JSON プロジェクト保存/読込。
- MIDI Note On、CC7/74/71、Program Change、クロック同期。

## 4. 検証済みビルドコマンド（Windows）

### Python（PySide6 + PortAudio）

```powershell
cd 01_python_qt
python -m pip install -r requirements.txt
python main.py --selftest
python -m PyInstaller --noconfirm --clean --onefile --windowed --name XU808-Python ^
  --icon app.ico --add-data "app.ico;." ^
  --add-binary "%PYTHON%\Lib\site-packages\_sounddevice_data\portaudio-binaries\libportaudio64bit.dll;_sounddevice_data\portaudio-binaries" ^
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

### 純 C（Win32 + waveOut、静的リンク）

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++（静的 Qt 6.8.1）

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C#（.NET WinForms、NuGet 不要）

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
```

### PHP（MVC ウェブ版）

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. ゼロ依存の確認方法

1. Python/JDK/.NET/Qt/Node が入っていないクリーンな Windows を用意し、EXE または配布フォルダだけをコピー。
2. 起動し、タスクマネージャーにインタプリタ/ランタイムのプロセスがないことを確認（C、Qt、C#）。
3. スペース/PLAY で既定の 808 グルーヴが鳴ること。同じキックを 20 回叩いても音が積み重ならないこと。
4. `XU808-*.exe --selftest` を実行し、960044 バイトの 16bit/48k ステレオ WAV と
   `SELFTEST OK` が生成されること。
5. ウィンドウを閉じた後、オーディオスレッド/プロセスが残らないこと。

## 6. 使い方

- スペース：再生/停止。1–8：パターン選択。Z X C V B N M A S D F G H J K L：パッド演奏。
- セルを左クリック：オン/オフ。右クリック：ベロシティ/アクセント/確率/マイクロ/ラチェット/条件。
- 左の M/S：ミュート/ソロ。パッド名クリックで試聴。
- 下部スライダー：マスター LP/レゾナンス/HP/ドライブ/音量。メーターはリアルタイム。
- プロジェクトは `.xu808`（JSON）で保存。WAV/MIDI は下部ボタンで書き出し。

## 7. リアルタイム実装の要点

- クロック：int64 `samplePos` が唯一の基準。UI タイマーは再描画のみ。
- イベント：ステップ、スウィング、マイクロ、ラチェットを絶対サンプル時刻に変換。
- チョーク：クローズド・ハイハットがオープンを 10ms フェードで停止。
- ボイス再利用：自然減衰またはスティーリングで即非アクティブ。4 秒の上限。
- マスターは左右で別々の biquad 状態を持ち、クロストークを防ぐ。
- オフライン書き出しはリアルタイムと同じ `render()` を使用。

## 8. FAQ

- **音が出ない**：既定の出力デバイスを確認。Java/Electron はデバイス権限が必要。
- **遅延が大きい**：Java Sound と Electron は OS の音声スタックに制限される。
- **ウイルス対策の警告**：単一ファイル化ツールは誤検出されやすい。除外設定を。
- **MIDI がない**：機器を接続して再起動。Electron は Web MIDI 権限が必要。

## 9. ディレクトリ

```
00_shared/ 仕様 · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. クレジット

作者：**徐某**。TR-808 は Roland の商標です。本プロジェクトは Roland と無関係です。
