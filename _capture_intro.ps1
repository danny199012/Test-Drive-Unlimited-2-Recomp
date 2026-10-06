# Screenshots the opening scene unattended so a cvar experiment can be judged
# without someone watching the window and manually grabbing shots.
#
# Usage (launch the game ~10s before running this):
#   powershell -ExecutionPolicy Bypass -File _capture_intro.ps1 -Tag A -Offsets 20,40,60,90,120
#
# -Tag     names the output files (_intro_<Tag>_<offset>s.png)
# -Offsets capture times in seconds after this script starts.
#
# Full-virtual-screen grabs are ~3800x2100 PNGs (7 MB each). A 1280px JPEG
# alongside each one is small enough to open directly.

param(
  [string]$Tag = 'run',
  [int[]]$Offsets = @(20, 40, 60, 90, 120)
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$dir = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'

$prev = 0
foreach ($t in $Offsets) {
  if ($t -lt $prev) { Write-Error "offsets must ascend (got $t after $prev)"; exit 1 }
  Start-Sleep -Seconds ($t - $prev)
  $prev = $t

  $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
  $file = Join-Path $dir ("_intro_{0}_{1}s.png" -f $Tag, $t)
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose()
  $bmp.Dispose()
  Write-Output "saved $file"

  # Matching downscaled JPEG: large enough to read on screen, small enough to open.
  $src = [System.Drawing.Image]::FromFile($file)
  $w = 1280
  $h = [int]($src.Height * ($w / $src.Width))
  $small = New-Object System.Drawing.Bitmap $w, $h
  $gs = [System.Drawing.Graphics]::FromImage($small)
  $gs.InterpolationMode = 'HighQualityBicubic'
  $gs.DrawImage($src, 0, 0, $w, $h)
  $jfile = [System.IO.Path]::ChangeExtension($file, '.jpg')
  $small.Save($jfile, [System.Drawing.Imaging.ImageFormat]::Jpeg)
  $gs.Dispose(); $small.Dispose(); $src.Dispose()
  Write-Output "saved $jfile"
}