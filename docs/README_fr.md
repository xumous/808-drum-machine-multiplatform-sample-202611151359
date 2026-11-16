# XU-808 Boîte à rythmes analogique 16 voix · par Xu Mou

> Une spécification, dix-sept stacks techniques, un seul son.
> Chaque son est synthétisé en temps réel (oscillateurs + bruit + filtres + enveloppes), sans aucun échantillon.

## 1. Présentation

XU-808 est une boîte à rythmes logicielle inspirée du comportement du TR-808 : 16 voix,
séquenceur 16/64 pas, horloge à l'échantillon, swing, probabilité, micro-timing, ratchet,
accent, mute/solo, filtre maître et drive, export WAV/MIDI, sauvegarde de projet, entrée MIDI
et horloge externe. Les enveloppes sont des fonctions analytiques du temps : chaque frappe
se termine et la voix est libérée — aucun empilement de sons.

La même spécification `00_shared/XU808_SPEC.md` est implémentée en Python, Java, C pur,
Qt C++, Electron, Android (Java), C#, Kotlin, JSP, JavaEE, VB.NET, Go, Rust, PHP, ASM x86 et Ruby.

## 2. Modules et appels

```
Thread UI ──écrit──> données Project (Pattern/Pad/Master/Song)
    │                      │ lecture seule
    │ snapshot 30fps       v
    └─────────────> Engine (séquenceur + pool de voix)
                            │ rendu par bloc
                            v
                    thread audio (waveOut/QAudioSink)
```

- **Model** : Project / Pattern / Step / PadParams / Master / Song — données pures.
- **DSP** : Biquad + Voice ; les 16 formules sont identiques dans tous les langages.
- **Engine** : `samplePos` (int64) est la seule horloge ; chaque événement devient un temps
  absolu en échantillons ; chaîne maître HP → LP → Drive → soft clip → volume.
- **Audio** : thread dédié haute priorité ; aucune allocation, verrou, I/O ni appel UI.
- **UI** : édition et rafraîchissement 30fps ; ne pilote jamais le séquenceur.

## 3. Fonctions

- 16 voix : BD, SD, LT, MT, HT, LC, MC, HC, RS, CL, CP, MA, CB, CY, OH, CH.
- Par voix : Tune / Decay / Tone / Snappy / Tail / Level / Pan, lissés.
- 16/64 pas ; par pas : on, vélocité, accent, probabilité, micro, ratchet, condition.
- Swing, longueur par piste (polymètre), chaîne de morceaux, changement de pattern à la mesure.
- Choke charleston ouvert/fermé, 4 voix par pad, vol de voix.
- HP/LP biquad maître, drive, limiteur doux, vumètres.
- Export WAV 16 bits/48 kHz, MIDI SMF Format 1, stems (certaines versions).
- Projet JSON versionné.
- MIDI Note On, CC7/74/71, Program Change, synchronisation d'horloge.

## 4. Commandes de compilation vérifiées (Windows)

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

### C pur (Win32 + waveOut, statique)

```powershell
cd 03_c_win32
windres app.rc -o app_res.o
gcc -O2 -DUNICODE -D_UNICODE -mwindows -o ..\dist\XU808-C.exe ^
  main.c ui.c engine.c dsp.c app_res.o ^
  -lwinmm -lcomdlg32 -lgdi32 -luser32 -lole32 -static -static-libgcc -lm
```

### Qt C++ (Qt 6.8.1 statique)

```powershell
cd 04_qt_cpp
C:\Qt\6.8.1\static-mingw\bin\qmake.exe XU808.pro
C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j4
```

### C# (.NET WinForms, sans NuGet)

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

### PHP (web MVC)

```powershell
cd 14_php
tools\php\php.exe -S 127.0.0.1:18081 -t .
```

## 5. Vérification zéro dépendance

1. Utiliser un Windows propre sans Python/JDK/.NET/Qt/Node ; copier seulement l'EXE ou le dossier.
2. Lancer : le gestionnaire de tâches ne doit montrer aucun interpréteur/runtime (C, Qt, C#).
3. Espace/PLAY : le groove 808 par défaut doit sonner ; frapper 20 fois le même kick sans accumulation.
4. `XU808-*.exe --selftest` doit produire un WAV stéréo 16 bits/48 kHz de 960044 octets et
   `SELFTEST OK`.
5. Fermer la fenêtre : aucun thread/processus audio ne doit rester.

## 6. Utilisation

- Espace : lecture/arrêt ; 1–8 : pattern ; Z X C V B N M A S D F G H J K L : pads.
- Clic gauche sur une cellule : on/off ; clic droit : vélocité/accent/probabilité/micro/ratchet/condition.
- M/S à gauche : mute/solo ; cliquer le nom d'un pad pour l'écouter.
- Curseurs du bas : LP/résonance/HP/drive/volume ; vumètres en direct.
- Sauvegarde `.xu808` (JSON) ; exports WAV/MIDI via les boutons.

## 7. Règles temps réel

- Horloge : `samplePos` int64 fait foi ; les timers UI ne font que rafraîchir.
- Événements : pas, swing, micro et ratchet convertis en temps absolus.
- Choke : le charleston fermé coupe l'ouvert avec un fondu de 10 ms.
- Recyclage : décroissance naturelle ou vol de voix ; plafond dur de 4 s.
- Effets maître : état biquad séparé par canal.
- Export hors ligne : même `render()` que le temps réel.

## 8. FAQ

- **Pas de son** : vérifier la sortie par défaut ; Java/Electron exigent l'autorisation du périphérique.
- **Latence élevée** : Java Sound et Electron sont limités par la pile audio de l'OS.
- **Alerte antivirus** : les packers mono-fichier sont souvent signalés ; ajouter une exception.
- **Pas de MIDI** : rebrancher l'appareil et redémarrer ; Electron demande l'autorisation Web MIDI.

## 9. Arborescence

```
00_shared · 01_python_qt · 02_java_swing · 03_c_win32 · 04_qt_cpp
05_electron · 06_android_java · 07_csharp · 08_kotlin · 09_jsp · 10_javaee
11_vbnet · 12_go · 13_rust · 14_php · 15_asm · 16_ruby · dist · docs
```

## 10. Crédits

Auteur : **Xu Mou (徐某)**. TR-808 est une marque de Roland ; ce projet n'est pas affilié à Roland.
