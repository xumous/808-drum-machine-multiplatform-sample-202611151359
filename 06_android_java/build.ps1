# =============================================================================
#  build.ps1 —— XU-808 鼓机 Android 版命令行构建脚本（徐某手写）
#
#  不依赖 Gradle：aapt2 编译资源 → aapt2 link 出未签名 APK → javac 编译 Java
#  → d8 转 dex → jar 塞进 APK → zipalign 对齐 → apksigner 签名（debug/release）。
#
#  用法（在项目目录或任意位置）：
#      powershell -ExecutionPolicy Bypass -File build.ps1
#  产物：
#      dist\XU808-debug.apk
#      dist\XU808-release.apk
# =============================================================================

$ErrorActionPreference = "Stop"

# ---- 固定环境（本机事实） ----
$Root      = Split-Path -Parent $MyInvocation.MyCommand.Path
$JdkHome   = "C:\jdk17-tmp\jdk-17.0.21+9"
$JdkBin    = Join-Path $JdkHome "bin"
$Sdk       = Join-Path $Root "_sdk"
$BuildTools = Join-Path $Sdk "build-tools\34.0.0"
$AndroidJar = Join-Path $Sdk "platforms\android-28\android.jar"
$Build     = Join-Path $Root "build"
$Dist      = Join-Path $Root "dist"
$Keystore  = Join-Path $Root "keystore"

$env:JAVA_HOME = $JdkHome
$env:ANDROID_HOME = $Sdk
$env:ANDROID_SDK_ROOT = $Sdk

# 统一执行并检查退出码：任何一步失败都立即中止
function Run($Desc, $Exe, [string[]]$Arguments) {
    Write-Host ">>> $Desc" -ForegroundColor Cyan
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Desc 失败，退出码 $LASTEXITCODE" }
}

Write-Host "=== XU-808 Android build (XuMou) ===" -ForegroundColor Yellow

# ---- 0. 清理中间目录 ----
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Build, "$Build\gen", "$Build\classes", "$Build\dex", $Dist, $Keystore | Out-Null

# ---- 1. 编译资源（XML/PNG → 扁平化 .flat 包） ----
Run "aapt2 compile" (Join-Path $BuildTools "aapt2.exe") @(
    "compile", "--dir", (Join-Path $Root "res"), "-o", "$Build\res.zip"
)

# ---- 2. 链接资源与清单，生成基础 APK，并输出 R.java ----
Run "aapt2 link" (Join-Path $BuildTools "aapt2.exe") @(
    "link",
    "--manifest", (Join-Path $Root "AndroidManifest.xml"),
    "-I", $AndroidJar,
    "-R", "$Build\res.zip",
    "-o", "$Build\base.apk",
    "--java", "$Build\gen",
    "--auto-add-overlay"
)

# ---- 3. 编译 Java（-source/-target 8，bootclasspath 指向 android.jar） ----
$srcFiles = @(Get-ChildItem (Join-Path $Root "src") -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$genFiles = @(Get-ChildItem "$Build\gen" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
if ($srcFiles.Count -eq 0) { throw "没有找到 Java 源文件" }
Write-Host ">>> javac（$($srcFiles.Count) 个源文件 + $($genFiles.Count) 个 R.java）" -ForegroundColor Cyan
Run "javac" (Join-Path $JdkBin "javac.exe") (@(
    "-encoding", "UTF-8", "-source", "8", "-target", "8",
    "-bootclasspath", $AndroidJar,
    "-d", "$Build\classes"
) + $srcFiles + $genFiles)

# ---- 4. 先把 class 打成 jar（Windows 命令行有长度上限，d8 直接接文件名会超限） ----
$classFiles = @(Get-ChildItem "$Build\classes" -Recurse -Filter *.class | ForEach-Object { $_.FullName })
Write-Host ">>> jar 打包 $($classFiles.Count) 个 class" -ForegroundColor Cyan
Run "jar class→jar" (Join-Path $JdkBin "jar.exe") @(
    "--create", "--file", "$Build\classes.jar", "-C", "$Build\classes", "."
)

# ---- 5. d8：class jar → classes.dex（min-api 28） ----
Write-Host ">>> d8" -ForegroundColor Cyan
Run "d8" (Join-Path $BuildTools "d8.bat") @(
    "--lib", $AndroidJar, "--min-api", "28", "--output", "$Build\dex", "$Build\classes.jar"
)

# ---- 6. 把 classes.dex 写进 APK（jar 工具有 ZIP 更新能力） ----
Copy-Item "$Build\base.apk" "$Build\unsigned.apk" -Force
Run "jar 写入 classes.dex" (Join-Path $JdkBin "jar.exe") @(
    "--update", "--file", "$Build\unsigned.apk", "-C", "$Build\dex", "classes.dex"
)

# ---- 6. 4 字节对齐（Android 安装器要求） ----
Run "zipalign" (Join-Path $BuildTools "zipalign.exe") @(
    "-f", "-p", "4", "$Build\unsigned.apk", "$Build\aligned.apk"
)

# ---- 7. 生成签名证书：debug（10000 天）与 release（365000 天 ≈ 1000 年） ----
$DebugKs = Join-Path $Keystore "xu808-debug.jks"
if (-not (Test-Path $DebugKs)) {
    Run "keytool 生成 debug 密钥" (Join-Path $JdkBin "keytool.exe") @(
        "-genkeypair", "-keystore", $DebugKs, "-alias", "xu808debug",
        "-storepass", "android", "-keypass", "android",
        "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
        "-dname", "CN=XuMou, OU=XU808, O=XuMou, L=Hangzhou, ST=Zhejiang, C=CN"
    )
}
$ReleaseKs = Join-Path $Keystore "xu808-release.jks"
if (-not (Test-Path $ReleaseKs)) {
    Run "keytool 生成 release 密钥（365000 天）" (Join-Path $JdkBin "keytool.exe") @(
        "-genkeypair", "-keystore", $ReleaseKs, "-alias", "xu808",
        "-storepass", "xu808808", "-keypass", "xu808808",
        "-keyalg", "RSA", "-keysize", "2048", "-validity", "365000",
        "-dname", "CN=XuMou, OU=XU808, O=XuMou, L=Hangzhou, ST=Zhejiang, C=CN"
    )
}

# ---- 8. 签名：debug 与 release 各一份 ----
Run "apksigner 签 debug" (Join-Path $BuildTools "apksigner.bat") @(
    "sign", "--ks", $DebugKs, "--ks-pass", "pass:android", "--key-pass", "pass:android",
    "--ks-key-alias", "xu808debug",
    "--out", (Join-Path $Dist "XU808-debug.apk"), "$Build\aligned.apk"
)
Run "apksigner 签 release" (Join-Path $BuildTools "apksigner.bat") @(
    "sign", "--ks", $ReleaseKs, "--ks-pass", "pass:xu808808", "--key-pass", "pass:xu808808",
    "--ks-key-alias", "xu808",
    "--out", (Join-Path $Dist "XU808-release.apk"), "$Build\aligned.apk"
)

# ---- 9. 校验签名与 zip 对齐 ----
Run "apksigner verify debug" (Join-Path $BuildTools "apksigner.bat") @(
    "verify", "--print-certs", (Join-Path $Dist "XU808-debug.apk")
)
Run "apksigner verify release" (Join-Path $BuildTools "apksigner.bat") @(
    "verify", "--print-certs", (Join-Path $Dist "XU808-release.apk")
)

Write-Host ""
Write-Host "=== 构建成功 ===" -ForegroundColor Green
Get-ChildItem $Dist -Filter *.apk | ForEach-Object {
    Write-Host ("  {0}  {1:N0} bytes" -f $_.FullName, $_.Length)
}
