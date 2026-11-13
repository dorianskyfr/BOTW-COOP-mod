<#
Installe le mod dans un dossier de donnees Ryujinx (ou un dossier de mods du meme
format), a partir de la derniere build (build\deploy\subsdk9).

  powershell -File tools\install-mod.ps1 -DataDir <dossier de donnees Ryujinx> [-Relay 192.168.1.20:47800]
  powershell -File tools\install-mod.ps1 -DataDir <dossier> -Uninstall

-DataDir : le dossier qui contient "mods", "bis", "system" (pour l'installation
           standard de Ryujinx : %APPDATA%\Ryujinx).
-Relay   : adresse IPv4:port du relais (tools\relay.py). Sans cette option le mod
           se connecte a 127.0.0.1:47800 (relais sur le meme PC).

Le script n'ecrit que dans <DataDir>\mods\contents\01007ef00011e000\botw-coop.
#>
param(
    [Parameter(Mandatory = $true)][string]$DataDir,
    [string]$Relay = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $DataDir)) { throw "Dossier introuvable : $DataDir" }
$mod = Join-Path $DataDir 'mods\contents\01007ef00011e000\botw-coop'

if ($Uninstall) {
    foreach ($f in @((Join-Path $mod 'exefs\subsdk9'), (Join-Path $mod 'romfs\botw-coop.txt'))) {
        if (Test-Path $f) { Remove-Item $f -Confirm:$false; Write-Output "retire : $f" }
    }
    exit 0
}

$src = Join-Path $project 'build\deploy\subsdk9'
if (-not (Test-Path $src)) { throw "Pas de build : lancer d'abord plugin\build.sh (voir README.md)" }
if ($Relay -ne '' -and $Relay -notmatch '^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$') { throw "Adresse de relais invalide : $Relay (attendu a.b.c.d:port)" }

New-Item -ItemType Directory -Force (Join-Path $mod 'exefs') | Out-Null
Copy-Item $src (Join-Path $mod 'exefs\subsdk9') -Force
Write-Output "installe : $(Join-Path $mod 'exefs\subsdk9')"

$cfg = Join-Path $mod 'romfs\botw-coop.txt'
if ($Relay -ne '') {
    New-Item -ItemType Directory -Force (Join-Path $mod 'romfs') | Out-Null
    "relay=$Relay" | Set-Content $cfg -Encoding ascii
    Write-Output "relais : $Relay ($cfg)"
} elseif (Test-Path $cfg) {
    Remove-Item $cfg -Confirm:$false
    Write-Output 'relais : 127.0.0.1:47800 (ancien fichier de configuration retire)'
} else {
    Write-Output 'relais : 127.0.0.1:47800 (par defaut)'
}
