# Generates resources/app.ico for QBucket (Windows).
#
# Same geometry as the master artwork io.github.theinvisible.QBucket.svg
# (256x256 design grid, scaled per bitmap size): a bucket with handle, band
# and rim on a rounded blue gradient square. Seven bitmap sizes (16…256) are
# packed as PNG payloads inside the ICO so Windows picks the right one at
# every UI scale. Pattern taken from xyz-profiler's make_icon.ps1.
#
# Run once after editing (PowerShell 5+/7 on Windows). The output is checked
# into git.

Add-Type -AssemblyName System.Drawing

function New-IconBitmap([int]$size) {
    $s = $size / 256.0
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.ScaleTransform([float]$s, [float]$s)

    # ---- Rounded-square background, diagonal gradient --------------------
    $r = 52; $x0 = 8; $y0 = 8; $w = 240; $d = $r * 2
    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bgPath.AddArc($x0, $y0, $d, $d, 180, 90)
    $bgPath.AddArc($x0 + $w - $d, $y0, $d, $d, 270, 90)
    $bgPath.AddArc($x0 + $w - $d, $y0 + $w - $d, $d, $d, 0, 90)
    $bgPath.AddArc($x0, $y0 + $w - $d, $d, $d, 90, 90)
    $bgPath.CloseFigure()
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush `
        (New-Object System.Drawing.PointF 8, 8), `
        (New-Object System.Drawing.PointF 248, 248), `
        ([System.Drawing.Color]::FromArgb(255, 0x3B, 0x82, 0xF6)), `
        ([System.Drawing.Color]::FromArgb(255, 0x1D, 0x4E, 0xD8))
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose(); $bgPath.Dispose()

    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)

    # ---- Handle (quadratic curve M70 96 Q128 22 186 96 as cubic) ---------
    if ($size -ge 24) {
        $handlePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 255, 255, 255)), ([float]10)
        $handlePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $handlePen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
        # Quadratic (P0,Q,P2) -> cubic control points P0+2/3(Q-P0), P2+2/3(Q-P2)
        $g.DrawBezier($handlePen, 70, 96, 108.67, 46.67, 147.33, 46.67, 186, 96)
        $handlePen.Dispose()
    }

    # ---- Body ------------------------------------------------------------
    $body = New-Object System.Drawing.Drawing2D.GraphicsPath
    $body.AddLine(64, 96, 84, 194)
    $body.AddBezier(84, 194, 85.33, 203.33, 90.67, 208, 100, 208)
    $body.AddLine(100, 208, 156, 208)
    $body.AddBezier(156, 208, 165.33, 208, 170.67, 203.33, 172, 194)
    $body.AddLine(172, 194, 192, 96)
    $body.CloseFigure()
    $bodyBrush = New-Object System.Drawing.SolidBrush $white
    $g.FillPath($bodyBrush, $body)
    $bodyBrush.Dispose(); $body.Dispose()

    # ---- Band (M74 144 Q128 162 182 144) ---------------------------------
    if ($size -ge 32) {
        $bandPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0x25, 0x63, 0xEB)), ([float]9)
        $bandPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $bandPen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
        $g.DrawBezier($bandPen, 74, 144, 110, 156, 146, 156, 182, 144)
        $bandPen.Dispose()
    }

    # ---- Rim + opening ---------------------------------------------------
    $rimBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xDB, 0xEA, 0xFE))
    $g.FillEllipse($rimBrush, 64, 78, 128, 36)
    $rimBrush.Dispose()
    $rimPen = New-Object System.Drawing.Pen $white, ([float]8)
    $g.DrawEllipse($rimPen, 64, 78, 128, 36)
    $rimPen.Dispose()
    $openBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(140, 0x1E, 0x40, 0xAF))
    $g.FillEllipse($openBrush, 78, 86, 100, 20)
    $openBrush.Dispose()

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$pngBuffers = @{}
foreach ($sz in $sizes) {
    $bmp = New-IconBitmap $sz
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBuffers[$sz] = $ms.ToArray()
    $ms.Dispose()
    $bmp.Dispose()
}

# ---- Pack ICO container with PNG payloads --------------------------------
$icoStream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $icoStream

$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]$sizes.Count)

$headerSize = 6 + 16 * $sizes.Count
$offset = $headerSize
foreach ($sz in $sizes) {
    $bytes = $pngBuffers[$sz]
    $wb = if ($sz -ge 256) { [byte]0 } else { [byte]$sz }
    $writer.Write([byte]$wb)
    $writer.Write([byte]$wb)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$bytes.Length)
    $writer.Write([UInt32]$offset)
    $offset += $bytes.Length
}
foreach ($sz in $sizes) {
    $writer.Write($pngBuffers[$sz])
}

$icoBytes = $icoStream.ToArray()
$writer.Dispose()
$icoStream.Dispose()

$out = Join-Path $PSScriptRoot "app.ico"
[System.IO.File]::WriteAllBytes($out, $icoBytes)
Write-Output "Wrote $out ($($icoBytes.Length) bytes, $($sizes.Count) sizes)"
