<#
Capture d'ecran de la fenetre principale d'un processus (pour voir ce que fait
le jeu pendant un test automatique). Ne capture que le rectangle de cette
fenetre. La fenetre est amenee au premier plan le temps de la capture, sinon
on photographierait ce qui la recouvre.

Usage (dot-source) :
  . tools\capture-window.ps1
  Save-WindowCapture -Process $p -Path build\runs\x.png
#>
Add-Type -AssemblyName System.Drawing
if (-not ('CoopWin32' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CoopWin32 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@
}

# Capture integree de Ryujinx (raccourci F8, rendu pris dans l'emulateur lui-meme).
# Utile quand la capture d'ecran classique ne voit pas l'image du jeu.
function Send-RyujinxScreenshotKey {
    param([System.Diagnostics.Process]$Process)
    Add-Type -AssemblyName System.Windows.Forms
    $Process.Refresh()
    $h = $Process.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return 'pas de fenetre' }
    [void][CoopWin32]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait('{F8}')
    return 'F8 envoye'
}

function Save-WindowCapture {
    param([System.Diagnostics.Process]$Process, [string]$Path, [int]$MaxWidth = 960, [string]$Method = 'PrintWindow')

    $Process.Refresh()
    $h = $Process.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return 'pas de fenetre' }

    if ([CoopWin32]::IsIconic($h)) { [void][CoopWin32]::ShowWindow($h, 9) }
    [void][CoopWin32]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 400

    $r = New-Object CoopWin32+RECT
    if (-not [CoopWin32]::GetWindowRect($h, [ref]$r)) { return 'GetWindowRect a echoue' }
    $w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
    if ($w -le 0 -or $hgt -le 0) { return "rectangle invalide ${w}x${hgt}" }

    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if ($Method -eq 'PrintWindow') {
        # PW_RENDERFULLCONTENT (2) : demande a DWM le contenu compose de la fenetre,
        # y compris les surfaces rendues par le GPU que la copie d'ecran GDI peut manquer.
        $hdc = $g.GetHdc()
        [void][CoopWin32]::PrintWindow($h, $hdc, 2)
        $g.ReleaseHdc($hdc)
    } else {
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
    }
    $g.Dispose()

    # Reduction pour garder des fichiers legers.
    if ($w -gt $MaxWidth) {
        $nh = [int]($hgt * $MaxWidth / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, $MaxWidth, $nh
        $bmp.Dispose(); $bmp = $small
    }
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return "$Path (${w}x${hgt})"
}
