Add-Type -AssemblyName System.Drawing
$pngPath = "C:\Users\ecst4ssy\Desktop\Zapret Private\resources\logo.png"
$icoPath = "C:\Users\ecst4ssy\Desktop\Zapret Private\src\app.ico"

$bmp = [System.Drawing.Bitmap]::FromFile($pngPath)
$resized = New-Object System.Drawing.Bitmap($bmp, 256, 256)
$hIcon = $resized.GetHicon()
$icon = [System.Drawing.Icon]::FromHandle($hIcon)

$fileStream = [System.IO.File]::Create($icoPath)
$icon.Save($fileStream)
$fileStream.Close()
$fileStream.Dispose()
$bmp.Dispose()
$resized.Dispose()
Write-Host "ICO generated successfully!"
