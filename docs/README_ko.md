# XU-808 16보이스 아날로그 드럼 머신 · 서모(徐某)

> 하나의 사양, 열일곱 개의 기술 스택, 하나의 소리.
> 모든 소리는 오실레이터 + 노이즈 + 필터 + 엔벨로프로 실시간 합성되며 샘플을 쓰지 않습니다.

## 1. 소개

XU-808은 TR-808의 회로 동작을 참고해 서모가 구현한 소프트웨어 드럼 머신입니다.
16보이스, 16/64스텝 시퀀서, 샘플 정밀 클록, 스윙, 확률, 마이크로 타이밍, 라쳇, 악센트,
뮤트/솔로, 마스터 필터와 드라이브, WAV/MIDI 내보내기, 프로젝트 저장/불러오기, MIDI 입력을
지원합니다. 엔벨로프는 시간의 해석 함수이므로 각 타격이 스스로 끝나고 보이스가 해제됩니다.
소리가 겹쳐 쌓이지 않습니다.

동일한 `00_shared/XU808_SPEC.md`를 Python, Java, 순수 C, Qt C++, Electron, Android(Java),
C#, Kotlin, JSP, JavaEE, VB.NET, Go, Rust, PHP, x86 ASM, Ruby로 구현했습니다.

## 2. 모듈과 호출 관계

```
UI 스레드 ──쓰기──> Project 데이터 (Pattern/Pad/Master/Song)
    │                     │ 읽기 전용
    │ 30fps 스냅샷        v
    └────────────> Engine (시퀀서 + 보이스 풀)
                          │ 블록 단위 렌더
                          v
                   오디오 콜백 스레드 (waveOut/QAudioSink)
```

- **Model**: Project / Pattern / Step / PadParams / Master / Song — 데이터만.
- **DSP**: Biquad + Voice. 16가지 합성식은 모든 언어에서 동일.
- **Engine**: int64 `samplePos`가 유일한 클록. 모든 이벤트를 절대 샘플 시각으로 변환.
  마스터 체인은 HP → LP → Drive → 소프트 클립 → 볼륨.
- **Audio**: 전용 고우선순위 스레드. 힙 할당·락·파일 IO·UI 호출 금지.
- **UI**: 데이터 편집과 30fps 갱신만. 시퀀서를 구동하지 않음.

## 3. 기능

- 16보이스: BD, SD, LT, MT, HT, LC, MC, HC, RS, CL, CP, MA, CB, CY, OH, CH.
- 보이스별 Tune / Decay / Tone / Snappy / Tail / Level / Pan, 실시간 스무딩.
- 16/64스텝. 스텝별 on, 벨로시티, 악센트, 확률, 마이크로, 라쳇, 조건.
- 스윙, 트랙별 길이(폴리미터), 송 체인, 마디 경계에서 패턴 전환.
- 오픈/클로즈 하이햇 초크, 패드당 4보이스, 보이스 스틸링.
- 마스터 HP/LP 바이쿼드, 드라이브, 소프트 리미터, 레벨 미터.
- 16bit/48kHz WAV, SMF Format 1 MIDI, 스템 WAV(일부).
- 버전이 있는 JSON 프로젝트 저장/불러오기.
- MIDI Note On, CC7/74/71, Program Change, 클록 동기화.

## 4. 검증된 빌드 명령(Windows)

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

### 순수 C (Win32 + waveOut, 정적 링크)

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++ (정적 Qt 6.8.1)

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C# (.NET WinForms, NuGet 없음)

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

### PHP (MVC 웹)

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. 무의존 실행 검증

1. Python/JDK/.NET/Qt/Node가 없는 깨끗한 Windows에 EXE 또는 배포 폴더만 복사.
2. 실행 후 작업 관리자에 인터프리터/런타임 프로세스가 없어야 함(C, Qt, C#).
3. Space/PLAY로 기본 808 그루브가 들려야 함. 같은 킥을 20번 쳐도 소리가 쌓이지 않아야 함.
4. `XU808-*.exe --selftest`가 960044바이트 16bit/48k 스테레오 WAV와 `SELFTEST OK`를 생성.
5. 창을 닫은 뒤 오디오 스레드/프로세스가 남지 않아야 함.

## 6. 사용법

- Space: 재생/정지, 1–8: 패턴 선택, Z X C V B N M A S D F G H J K L: 패드 연주.
- 셀 왼쪽 클릭: on/off, 오른쪽 클릭: 벨로시티/악센트/확률/마이크로/라쳇/조건.
- 왼쪽 M/S: 뮤트/솔로, 패드 이름 클릭으로 미리 듣기.
- 하단 슬라이더: 마스터 LP/공진/HP/드라이브/볼륨, 레벨 미터 실시간.
- 프로젝트는 `.xu808`(JSON)로 저장, WAV/MIDI는 하단 버튼으로 내보내기.

## 7. 실시간 구현 규칙

- 클록: int64 `samplePos`가 유일한 기준. UI 타이머는 다시 그리기만.
- 이벤트: 스텝·스윙·마이크로·라쳇을 절대 샘플 시각으로 변환.
- 초크: 클로즈 하이햇이 오픈 하이햇을 10ms 페이드로 끊음.
- 보이스 재활용: 자연 감쇠 또는 스틸링 시 즉시 비활성. 4초 하드 캡.
- 마스터는 좌우 별도 biquad 상태로 크로스토크 방지.
- 오프라인 내보내기는 실시간과 같은 `render()` 사용.

## 8. FAQ

- **소리 없음**: 기본 출력 장치 확인. Java/Electron은 장치 권한 필요.
- **지연 큼**: Java Sound와 Electron은 OS 오디오 스택 한계.
- **백신 경고**: 단일 파일 패커는 오탐이 잦음. 예외 처리 필요.
- **MIDI 없음**: 장치를 다시 연결하고 재시작. Electron은 Web MIDI 권한 필요.

## 9. 디렉터리

```
00_shared · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. 크레딧

제작: **서모(徐某)**. TR-808은 Roland의 상표이며 본 프로젝트는 Roland와 무관합니다.
