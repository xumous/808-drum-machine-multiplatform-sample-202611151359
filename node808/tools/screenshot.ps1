param([string]$Out = "$env:TEMP\n808shot.png", [int]$Wait = 6)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$p = Start-Process -FilePath "c:\Users\Happy\Pictures\watermark\guji808_202611142022\node808\node_modules\electron\dist\electron.exe" -ArgumentList "." -WorkingDirectory "c:\Users\Happy\Pictures\watermark\guji808_202611142022\node808" -PassThru
Start-Sleep -Seconds $Wait
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output ("saved " + $Out + " size=" + $b.Width + "x" + $b.Height + " alive=" + (-not $p.HasExited))
if (-not $p.HasExited) { $p.Kill() }
Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force
