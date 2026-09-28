$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRoot = Split-Path -Parent $PSScriptRoot
$iconPath = Join-Path $taskRoot 'assets\monitor.ico'
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = [System.Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size / 256.0, $size / 256.0)
    $shield = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $shield.AddLine(128, 12, 230, 52)
    $shield.AddLine(230, 52, 230, 128)
    $shield.AddBezier(230, 128, 230, 185, 176, 227, 128, 244)
    $shield.AddBezier(128, 244, 80, 227, 26, 185, 26, 128)
    $shield.AddLine(26, 128, 26, 52)
    $shield.CloseFigure()
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(26, 12), [System.Drawing.Point]::new(230, 244), [System.Drawing.Color]::FromArgb(22, 114, 239), [System.Drawing.Color]::FromArgb(35, 74, 185))
    $graphics.FillPath($brush, $shield)
    $paper = [System.Drawing.Point[]]@([System.Drawing.Point]::new(76, 58), [System.Drawing.Point]::new(141, 58), [System.Drawing.Point]::new(178, 94), [System.Drawing.Point]::new(178, 194), [System.Drawing.Point]::new(76, 194))
    $graphics.FillPolygon([System.Drawing.Brushes]::White, $paper)
    $fold = [System.Drawing.Point[]]@([System.Drawing.Point]::new(141, 58), [System.Drawing.Point]::new(141, 94), [System.Drawing.Point]::new(178, 94))
    $foldBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(187, 214, 255))
    $graphics.FillPolygon($foldBrush, $fold)
    $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(33, 97, 207), 13)
    $pen.StartCap = $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $tick = [System.Drawing.Point[]]@([System.Drawing.Point]::new(98, 143), [System.Drawing.Point]::new(119, 164), [System.Drawing.Point]::new(157, 121))
    $graphics.DrawLines($pen, $tick)
    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images.Add($stream.ToArray())
    $stream.Dispose(); $pen.Dispose(); $foldBrush.Dispose(); $brush.Dispose(); $shield.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
$file = [System.IO.File]::Create($iconPath)
$writer = [System.IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($bytes in $images) { $writer.Write($bytes) }
} finally { $writer.Dispose(); $file.Dispose() }
[System.IO.File]::WriteAllBytes((Join-Path $taskRoot 'assets\monitor.png'), $images[$images.Count - 1])
Write-Output "Generated $iconPath"
