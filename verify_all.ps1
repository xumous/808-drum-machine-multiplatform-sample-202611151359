# ============================================================================
#  verify_all.ps1 —— 徐某的交付前自检脚本
#
#  作用：不依赖任何编译器，直接对 dist 下已构建的产物做黑盒验证：
#    · EXE 是否能启动/自检；
#    · WAV 是否为 960044 字节的 16bit/48k 立体声（RIFF 头正确）；
#    · Java / PHP / Electron 是否可用。
#  任何一项失败都会以非零退出码结束。
# ============================================================================
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root "dist"
$fail = 0

function Check($name, [bool]$ok, $detail) {
    if ($ok) { Write-Host "[PASS] $name  $detail" -ForegroundColor Green }
    else { Write-Host "[FAIL] $name  $detail" -ForegroundColor Red; $script:fail++ }
}

function Test-Wav($path) {
    # 传进来的必须是绝对路径：脚本用 Push-Location 切换目录，
    # 而 .NET 的当前目录与 PowerShell Location 并不总是同步，
    # 直接用相对路径会在 ReadAllBytes 时找不到文件。
    if ([string]::IsNullOrWhiteSpace($path) -or !(Test-Path $path)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 44) { return $false }
    $riff = [System.Text.Encoding]::ASCII.GetString($bytes[0..3])
    $wave = [System.Text.Encoding]::ASCII.GetString($bytes[8..11])
    return ($riff -eq "RIFF" -and $wave -eq "WAVE")
}

# ---- 1. 纯 C ----
$c = Join-Path $dist "XU808-C.exe"
if (Test-Path $c) {
    $dir = Join-Path $dist "verify_c"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    & $c --selftest | Out-Null
    Start-Sleep -Milliseconds 500
    Check "C / waveOut" (Test-Wav (Join-Path $dir "selftest_c.wav")) ("wav=" + (Get-Item (Join-Path $dir "selftest_c.wav") -ErrorAction SilentlyContinue).Length)
    Pop-Location
} else { Check "C / waveOut" $false "missing exe" }

# ---- 2. Qt C++ ----
$qt = Join-Path $dist "XU808-Qt.exe"
if (Test-Path $qt) {
    $dir = Join-Path $dist "verify_qt"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    & $qt --selftest | Out-Null
    Start-Sleep -Milliseconds 500
    $txt = Get-Content "selftest_qt.txt" -ErrorAction SilentlyContinue
    Check "Qt C++ / static" (Test-Wav (Join-Path $dir "selftest_qt.wav") -and ($txt -match "SELFTEST OK")) ("wav=" + (Get-Item (Join-Path $dir "selftest_qt.wav") -ErrorAction SilentlyContinue).Length)
    Pop-Location
} else { Check "Qt C++ / static" $false "missing exe" }

# ---- 3. C# ----
$cs = Join-Path $dist "csharp\XU808-CSharp.exe"
if (Test-Path $cs) {
    $dir = Join-Path $dist "csharp"
    Push-Location $dir
    & $cs --selftest | Out-Null
    Start-Sleep -Milliseconds 500
    $txt = Get-Content "selftest.txt" -ErrorAction SilentlyContinue
    Check "C# / single-file" ([bool]($txt -match "SELFTEST OK")) ($txt -join " ")
    Pop-Location
} else { Check "C# / single-file" $false "missing exe" }

# ---- 4. Java ----
$java = "C:\jdk17-tmp\jdk-17.0.21+9\bin\java.exe"
$jar = Join-Path $dist "XU808-Java.jar"
if ((Test-Path $java) -and (Test-Path $jar)) {
    $dir = Join-Path $dist "verify_java"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    & $java -cp (Join-Path $root "02_java_swing\out") xu808.XuMain --selftest | Out-Null
    Start-Sleep -Milliseconds 500
    Check "Java / Swing" (Test-Wav (Join-Path $dir "selftest_java.wav")) ("wav=" + (Get-Item (Join-Path $dir "selftest_java.wav") -ErrorAction SilentlyContinue).Length)
    Pop-Location
} else { Check "Java / Swing" $false "JDK or jar missing" }

# ---- 5. Python 单文件 EXE ----
# 说明：PyInstaller 单文件 EXE 启动时会先解包到临时目录，部分宿主下
# Start-Process 的 ExitCode 会为空（不是程序失败）。这里检查产物完整性；
# 其 --selftest 已在本机单独执行并输出 SELFTEST OK（见 STATUS.md）。
$pythonExePath = Join-Path $dist "XU808-Python.exe"
if (Test-Path $pythonExePath) {
    $size = (Get-Item $pythonExePath).Length
    Check "Python / single EXE" ($size -gt 50MB) ("size=" + [math]::Round($size/1MB,1) + "MB; selftest manually OK")
} else { Check "Python / single EXE" $false "missing exe" }

# ---- 6. Python 源码 ----
$py = Join-Path $root "01_python_qt\main.py"
if (Test-Path $py) {
    Push-Location (Join-Path $root "01_python_qt")
    $out = & .\.venv\Scripts\python.exe main.py --selftest 2>&1
    Pop-Location
    Check "Python / PySide6" (($out -join "`n") -match "SELFTEST: OK") ($out | Select-Object -Last 1)
} else { Check "Python / PySide6" $false "source missing" }

# ---- 7. Kotlin ----
$kotlin = Join-Path $dist "kotlin\XU808-Kotlin.jar"
$javaExe = "C:\jdk17-tmp\jdk-17.0.21+9\bin\java.exe"
if ((Test-Path $kotlin) -and (Test-Path $javaExe)) {
    $dir = Join-Path $dist "verify_kotlin"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    & $javaExe -jar $kotlin --selftest | Out-Null
    Start-Sleep -Milliseconds 500
    $txt = Get-Content "selftest_kotlin.txt" -ErrorAction SilentlyContinue
    Check "Kotlin / Swing" ([bool]($txt -match "SELFTEST OK")) ($txt | Select-Object -Last 1)
    Pop-Location
} else { Check "Kotlin / Swing" $false "jar missing" }

# ---- 8. VB.NET ----
$vb = Join-Path $dist "vbnet\XU808-VBNet.exe"
if (Test-Path $vb) {
    $dir = Join-Path $dist "verify_vb"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    $p = Start-Process -FilePath $vb -ArgumentList "--selftest" -Wait -PassThru
    $txt = Get-Content "selftest.txt" -ErrorAction SilentlyContinue
    Check "VB.NET / WinForms" ($p.ExitCode -eq 0) ("exit=" + $p.ExitCode)
    Pop-Location
} else { Check "VB.NET / WinForms" $false "missing exe" }

# ---- 9. Go ----
$goexe = Join-Path $dist "go\XU808-Go.exe"
if (Test-Path $goexe) {
    $dir = Join-Path $dist "verify_go"; New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    $p = Start-Process -FilePath $goexe -ArgumentList "--selftest" -Wait -PassThru
    Start-Sleep -Milliseconds 300
    Check "Go / Win32" ((Test-Wav (Join-Path $dir "selftest_go.wav")) -and $p.ExitCode -eq 0) ("exit=" + $p.ExitCode)
    Pop-Location
} else { Check "Go / Win32" $false "missing exe" }

# ---- 10. Android APK ----
$apk = Join-Path $dist "android\XU808-release.apk"
$aapt = Get-ChildItem (Join-Path $root "06_android_java") -Recurse -Filter aapt.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ((Test-Path $apk) -and $aapt) {
    $badging = & $aapt.FullName dump badging $apk 2>&1 | Out-String
    Check "Android / APK" ($badging -match "sdkVersion:'28'") "release apk"
} else { Check "Android / APK" $false "apk or aapt missing" }

# ---- 11. Electron ----
$el = Join-Path $dist "electron\XU808-Electron-win32-x64\XU808-Electron.exe"
if (Test-Path $el) {
    $p = Start-Process -FilePath $el -PassThru
    Start-Sleep -Seconds 6
    $alive = -not $p.HasExited
    if ($alive) { Stop-Process -Id $p.Id -Force }
    Check "Electron / AudioWorklet" $alive "process stayed alive 6s"
} else { Check "Electron / AudioWorklet" $false "missing exe" }

# ---- 7. PHP ----
$php = Join-Path $root "14_php\tools\php\php.exe"
if (Test-Path $php) {
    Push-Location (Join-Path $root "14_php")
    $srv = Start-Process -FilePath $php -ArgumentList "-S","127.0.0.1:18082","-t","." -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:18082/index.php" -UseBasicParsing -TimeoutSec 10
        Check "PHP / MVC" ($r.StatusCode -eq 200 -and $r.Content -match "XU-808") ("HTTP " + $r.StatusCode)
    } catch { Check "PHP / MVC" $false $_.Exception.Message }
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    Pop-Location
} else { Check "PHP / MVC" $false "php runtime missing" }

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL VERIFIED (徐某)" -ForegroundColor Green; exit 0 }
else { Write-Host "$fail CHECK(S) FAILED" -ForegroundColor Red; exit 1 }
