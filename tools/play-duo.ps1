<#
Session a deux instances pour jouer a la main (pas de script d'entrees, pas
d'arret automatique). Lance le relais et deux fenetres Ryujinx, chacune sur son
dossier de donnees de test, puis rend la main.

Usage :
  powershell -File tools\play-duo.ps1 [-P1 t_duo7_p1] [-P2 t_duo7_p2] [-NoBuild] [-Defines '']
Sorties console : build\runs\play_p1.txt, play_p2.txt, play_relay.log (hors git).
Pour tout arreter : powershell -File tools\play-duo.ps1 -Stop
#>
param(
    [string]$P1 = 't_play_p1',
    [string]$P2 = 't_play_p2',
    [switch]$NoBuild,
    [string]$Defines = '',
    [switch]$Stop,
    # Une seule fenetre (joueur 1), pour les essais d'affichage.
    [switch]$Solo
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
# Emplacement de Ryujinx : variable d'environnement COOP_RYUJINX, sinon Documents\Emu-switch.
$ryujinx = if ($env:COOP_RYUJINX) { $env:COOP_RYUJINX } else { Join-Path $env:USERPROFILE 'Documents\Emu-switch\Ryujinx.exe' }
$game    = 'F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild\The Legend of Zelda Breath of the Wild [01007EF00011E000][v0].nsp'
$python  = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
$runs    = Join-Path $project 'build\runs'
$pidFile = Join-Path $runs 'play_pids.txt'
New-Item -ItemType Directory -Force $runs | Out-Null

if ($Stop) {
    if (Test-Path $pidFile) {
        foreach ($id in (Get-Content $pidFile)) {
            $proc = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
            if ($proc -and $proc.ProcessName -match 'Ryujinx|python') {
                Stop-Process -Id $proc.Id -Confirm:$false
                Write-Output "arrete : $($proc.ProcessName) $($proc.Id)"
            }
        }
        Clear-Content $pidFile
    }
    exit 0
}

if (-not $NoBuild) {
    $env:MSYSTEM = 'MSYS'
    $env:COOP_PROJECT = $project
    $env:COOP_DEFINES = $Defines
    & C:\msys64\usr\bin\bash.exe -lc 'IFS=; P=$(cygpath -u $COOP_PROJECT); bash $P/plugin/build.sh $COOP_PROJECT > /tmp/coop_build.log 2>&1; r=$?; grep -E -A6 error: /tmp/coop_build.log | head -n 40; exit $r'
    $rc = $LASTEXITCODE
    Remove-Item Env:COOP_DEFINES -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Output "BUILD ECHOUE ($rc)"; exit 1 }
}

$relayLog = Join-Path $runs 'play_relay.log'
Remove-Item $relayLog -ErrorAction SilentlyContinue -Confirm:$false
$relay = Start-Process $python -ArgumentList ('-u "' + (Join-Path $project 'tools\relay.py') + '" --log "' + $relayLog + '"') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if ($relay.HasExited) { Write-Output 'LE RELAIS NE DEMARRE PAS (port 47800 deja pris ?)'; exit 1 }
$ids = @($relay.Id)

$n = 0
$players = @($P1, $P2)
if ($Solo) { $players = @($P1) }
foreach ($player in $players) {
    $n++
    $dataDir = Join-Path $project "build\ryu\$player"
    # Dossier absent : copie de la sauvegarde de test en monde ouvert (build\ryu\t_lv1).
    if (-not (Test-Path $dataDir)) {
        . (Join-Path $PSScriptRoot 'new-test-dir.ps1')
        $dataDir = New-CoopTestDirFromSave -Project $project -Name $player -From 't_lv1'
    }
    if (-not (Test-Path (Join-Path $dataDir 'system\prod.keys'))) { Write-Output "Dossier de test incomplet : $dataDir"; exit 1 }
    $modDir = Join-Path $dataDir 'mods\contents\01007ef00011e000\botw-coop\exefs'
    New-Item -ItemType Directory -Force $modDir | Out-Null
    Copy-Item (Join-Path $project 'build\deploy\subsdk9') (Join-Path $modDir 'subsdk9') -Force
    # Fichiers generes depuis la RomFS de l'utilisateur (liste blanche, Link anime), hors git.
    $romfsSrc = Join-Path $project 'build\deploy\romfs'
    if (Test-Path $romfsSrc) {
        $romfsDst = Join-Path (Split-Path $modDir -Parent) 'romfs'
        New-Item -ItemType Directory -Force $romfsDst | Out-Null
        Copy-Item (Join-Path $romfsSrc '*') $romfsDst -Recurse -Force
    } else {
        Write-Output 'Attention : pas de build\deploy\romfs (voir README, « Liste blanche ») : villageois au lieu de Link, aucun drapeau partage.'
    }
    $out = Join-Path $runs "play_p$n.txt"
    $p = Start-Process -FilePath $ryujinx -ArgumentList ('--root-data-dir "' + $dataDir + '" "' + $game + '"') -PassThru -RedirectStandardOutput $out -RedirectStandardError (Join-Path $runs "play_p$n.err.txt")
    $ids += $p.Id
    Write-Output "joueur $n : PID $($p.Id), donnees $player"
    if ($n -eq 1) { Start-Sleep -Seconds 8 }
}
$ids | Set-Content $pidFile
Write-Output "relais PID $($relay.Id) ; pour tout arreter : tools\play-duo.ps1 -Stop"
