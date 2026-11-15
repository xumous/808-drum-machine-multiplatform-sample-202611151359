param(
  [string]$Exe = "C:\Users\Happy\Pictures\watermark\guji808_202611142022\dist\808C.exe",
  [int]$Seconds = 90,
  [string]$Out = "C:\Users\Happy\Pictures\watermark\guji808_202611142022\verify_c_stress.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
Get-Process 808C -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process 808Node,node808,electron -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 900
$p = Start-Process -FilePath $Exe -PassThru
for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break } }
$h = $p.MainWindowHandle
Write-Output ("hwnd=" + $h)
if ($h -eq 0) { exit 1 }
[W]::SetForegroundWindow($h) | Out-Null
[W]::PostMessage($h, 0x100, [IntPtr]0x20, [IntPtr]0) | Out-Null
[W]::PostMessage($h, 0x101, [IntPtr]0x20, [IntPtr]0) | Out-Null
Start-Sleep -Seconds $Seconds
$p.Refresh()
Write-Output ("alive=" + (-not $p.HasExited) + " cpuSec=" + $p.TotalProcessorTime.TotalSeconds)
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap([int]$b.Width), ([int]$b.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
[W]::PostMessage($h, 0x100, [IntPtr]0x20, [IntPtr]0) | Out-Null
[W]::PostMessage($h, 0x101, [IntPtr]0x20, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 600
if (-not $p.HasExited) { $p.Kill() }
Write-Output "done"
