# build-wasm.ps1 —— 用 Zig 自带的 clang 把 core808 编译成 wasm32-wasi 模块
#
#   powershell -ExecutionPolicy Bypass -File tools\build-wasm.ps1 [-Zig <zig.exe>]
#
# 说明：
#  * core808.c / core808_io.c 一行不改，直接参与编译（含 WAV/MIDI/工程读写）。
#  * 采用 wasi reactor 模型，导出 _initialize / memory / 全部符号。
#  * AudioWorklet 里没有文件系统，WASI 的 fd_* 由 JS 侧桩函数实现（见 app/wasi-shim.js）。
param(
    [string]$Zig = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..")
$repo = Resolve-Path (Join-Path $root "..")
$core = Join-Path $repo "core808"

if (-not $Zig) {
    $candidates = @(
        (Join-Path $root ".tools\zig\zig.exe")
    )
    $envDir = $env:ZIG
    if ($envDir) { $candidates = @($envDir) + $candidates }
    Get-ChildItem (Join-Path $env:TEMP "zigdist\x\*\zig.exe") -ErrorAction SilentlyContinue |
        ForEach-Object { $candidates = $candidates + $_.FullName }
    $cmd = Get-Command zig -ErrorAction SilentlyContinue
    if ($cmd) { $Zig = $cmd.Source }
    else {
        foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { $Zig = $c; break } }
    }
}
if (-not $Zig -or -not (Test-Path $Zig)) {
    throw "未找到 zig.exe。请安装 Zig 0.14+ 并把 zig.exe 放到 node808\tools\zig\zig.exe，或传入 -Zig <path>。"
}

$out = Join-Path $root "app\wasm\core808.wasm"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null

$sources = @(
    (Join-Path $core "core808.c"),
    (Join-Path $core "core808_io.c"),
    (Join-Path $root "src\n808_glue.c")
)
foreach ($s in $sources) {
    if (-not (Test-Path $s)) { throw "缺少源文件：$s" }
}

$exports = @(
    "c808_create", "c808_destroy", "c808_render", "c808_push_midi",
    "c808_report_cpu", "c808_report_xrun",
    "c808_export_wav", "c808_export_midi",
    "n808_api", "n808_pack_snapshot", "n808_snap_size", "n808_project_size",
    "n808_project_out", "n808_project_in", "n808_song_out",
    "malloc", "free"
)
$zargs = @(
    "cc",
    "-target", "wasm32-wasi",
    "-O2",
    "-DNDEBUG",
    "-mexec-model=reactor",
    "-I", $core,
    "-Wl,--export-memory",
    "-Wl,-z,stack-size=131072",
    "-o", $out
) + ($exports | ForEach-Object { "-Wl,--export=$_" }) + $sources

Write-Host "[build-wasm] $Zig $($zargs -join ' ')"
& $Zig @zargs
if ($LASTEXITCODE -ne 0) { throw "zig cc 失败 (exit $LASTEXITCODE)" }

$len = (Get-Item $out).Length
Write-Host ("[build-wasm] OK -> {0} ({1:N0} bytes)" -f $out, $len)

# 打印导入表，便于维护 wasi-shim.js
$inspect = @'
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const m = new WebAssembly.Module(buf);
const groups = {};
for (const imp of WebAssembly.Module.imports(m)) {
  if (imp.kind !== 'function') continue;
  (groups[imp.module] = groups[imp.module] || []).push(imp.name);
}
for (const k of Object.keys(groups)) console.log('imports[' + k + ']: ' + groups[k].join(', '));
const ex = WebAssembly.Module.exports(m).map(e => e.name);
console.log('exports: ' + ex.filter(n => /^(c808_|n808_|_initialize|memory|malloc|free)/.test(n)).join(', '));
'@
$tmp = Join-Path $env:TEMP "n808_inspect.cjs"
Set-Content -Path $tmp -Value $inspect -Encoding UTF8
node $tmp $out
