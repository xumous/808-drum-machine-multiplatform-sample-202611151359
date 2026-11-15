param(
  [string]$Exe = "c:\Users\Happy\Pictures\watermark\guji808_202611142022\node808\dist\node808-portable.exe",
  [string]$OutA = "c:\Users\Happy\Pictures\watermark\guji808_202611142022\node808\tools\packA.png",
  [string]$OutB = "c:\Users\Happy\Pictures\watermark\guji808_202611142022\node808\tools\packB.png",
  [int]$Wait = 10
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Get-Process electron,node808,node808-portable -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

function Save-Shot([string]$path) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap([int]$b.Width), ([int]$b.Height)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}

$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds $Wait
$alive1 = -not $p.HasExited
Save-Shot $OutA
Start-Sleep -Seconds 5
$alive2 = -not $p.HasExited
Save-Shot $OutB
Write-Output ("alive1=$alive1 alive2=$alive2 pid=" + $p.Id)
if (-not $p.HasExited) { $p.Kill() }
Get-Process node808,node808-portable,electron -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "done"
