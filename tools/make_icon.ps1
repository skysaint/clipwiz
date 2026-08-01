# make_icon.ps1 - generates src/clipwiz.ico
# Draws a 256x256 master bitmap with GDI+, scales it down to each target size,
# then writes the ICO container by hand (BMP entries, 32bpp with alpha).
# Only 16/24/32/48/64 are included so the icon resource stays small.
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 decodes BOM-less
# scripts as ANSI, which corrupts non-ASCII comments and breaks parsing.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$outPath = Join-Path $PSScriptRoot '..\src\clipwiz.ico'
$outPath = [System.IO.Path]::GetFullPath($outPath)

function New-RoundedPath([int]$x, [int]$y, [int]$w, [int]$h, [int]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-MasterBitmap {
    $size = 256
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded blue plate
    $plate = New-RoundedPath 0 0 255 255 52
    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $c1 = [System.Drawing.Color]::FromArgb(255, 74, 144, 246)
    $c2 = [System.Drawing.Color]::FromArgb(255, 23, 78, 199)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 90.0)
    $g.FillPath($brush, $plate)
    $brush.Dispose()
    $plate.Dispose()

    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)

    # clipboard body
    $body = New-RoundedPath 62 66 132 140 16
    $g.FillPath($white, $body)
    $body.Dispose()

    # clip on top
    $clip = New-RoundedPath 100 46 56 34 10
    $g.FillPath($white, $clip)
    $clip.Dispose()
    $white.Dispose()

    # content lines on the body
    $blue = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 37, 99, 235))
    $g.FillRectangle($blue, 82, 106, 92, 14)
    $g.FillRectangle($blue, 82, 134, 92, 14)
    $g.FillRectangle($blue, 82, 162, 58, 14)
    $blue.Dispose()

    $g.Dispose()
    return $bmp
}

function Get-ScaledBitmap([System.Drawing.Bitmap]$master, [int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.DrawImage($master, (New-Object System.Drawing.Rectangle(0, 0, $size, $size)))
    $g.Dispose()
    return $bmp
}

function Get-BgraBottomUp([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width
    $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = [Math]::Abs($data.Stride)
    $raw = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($data)

    $rowBytes = $w * 4
    $out = New-Object byte[] ($rowBytes * $h)
    for ($y = 0; $y -lt $h; $y++) {
        $src = ($h - 1 - $y) * $stride
        [Array]::Copy($raw, $src, $out, $y * $rowBytes, $rowBytes)
    }
    # the leading comma keeps PowerShell from unrolling byte[] into Object[]
    return , $out
}

$sizes = @(16, 24, 32, 48, 64)
$master = New-MasterBitmap
$images = @()

foreach ($s in $sizes) {
    $scaled = Get-ScaledBitmap $master $s
    $pixels = [byte[]](Get-BgraBottomUp $scaled)
    $scaled.Dispose()

    $maskRow = [int][Math]::Floor(($s + 31) / 32) * 4
    $maskSize = $maskRow * $s

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40)                          # biSize
    $bw.Write([int32]$s)                           # biWidth
    $bw.Write([int32]($s * 2))                     # biHeight = XOR + AND
    $bw.Write([uint16]1)                           # biPlanes
    $bw.Write([uint16]32)                          # biBitCount
    $bw.Write([uint32]0)                           # biCompression = BI_RGB
    $bw.Write([uint32]($pixels.Length + $maskSize))
    $bw.Write([int32]0)
    $bw.Write([int32]0)
    $bw.Write([uint32]0)
    $bw.Write([uint32]0)
    $bw.Write($pixels)
    $bw.Write([byte[]](New-Object byte[] $maskSize))
    $bw.Flush()
    $images += , @{ Size = $s; Bytes = $ms.ToArray() }
    $bw.Dispose()
    $ms.Dispose()
}
$master.Dispose()

$fs = New-Object System.IO.FileStream($outPath, [System.IO.FileMode]::Create)
$writer = New-Object System.IO.BinaryWriter($fs)
$writer.Write([uint16]0)                 # reserved
$writer.Write([uint16]1)                 # type = icon
$writer.Write([uint16]$images.Count)

$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $s = [int]$img.Size
    $dim = if ($s -ge 256) { 0 } else { $s }
    $writer.Write([byte]$dim)
    $writer.Write([byte]$dim)
    $writer.Write([byte]0)               # palette count
    $writer.Write([byte]0)               # reserved
    $writer.Write([uint16]1)             # planes
    $writer.Write([uint16]32)            # bit count
    $writer.Write([uint32]$img.Bytes.Length)
    $writer.Write([uint32]$offset)
    $offset += $img.Bytes.Length
}
foreach ($img in $images) { $writer.Write([byte[]]$img.Bytes) }
$writer.Flush()
$writer.Dispose()
$fs.Dispose()

$info = Get-Item $outPath
Write-Host ("icon written: {0} ({1} bytes, {2} sizes)" -f $info.FullName, $info.Length, $images.Count)
