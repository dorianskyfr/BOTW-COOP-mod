<#
Session pilotee pas a pas : build, relais, une instance du jeu (dossier de test
neuf ou existant), puis rend la main. Le jeu se pilote ensuite avec
  python tools\drive.py <Name> <commandes...>
Arret : powershell -File tools\run-live.ps1 -Name <Name> -Stop

Usage :
  powershell -File tools\run-live.ps1 -Name live1 [-Dir t_live1] [-NoBuild] [-Defines '-DCOOP_REMOTE_PAD']
Sorties : build\runs\<Name>_game.txt, <Name>_relay.log, <Name>_pilot.log (hors git).
#>
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Dir = '',
    # Second joueur : nom de son dossier de test (vide = une seule instance).
    [string]$Dir2 = '',
    # Dossier de test dont on reprend la sauvegarde quand un dossier doit etre cree.
    [string]$From = '',
    [switch]$NoBuild,
    [string]$Defines = '-DCOOP_REMOTE_PAD',
    # Ecran ou ranger les fenetres de jeu (1 = principal, 0 = ne pas y toucher).
    [int]$Screen = 0,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
# Emplacement de Ryujinx : variable d'environnement COOP_RYUJINX, sinon Documents\Emu-switch.
$ryujinx = if ($env:COOP_RYUJINX) { $env:COOP_RYUJINX } else { Join-Path $env:USERPROFILE 'Documents\Emu-switch\Ryujinx.exe' }
$game    = 'F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild\The Legend of Zelda Breath of the Wild [01007EF00011E000][v0].nsp'
$python  = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
$runs    = Join-Path $project 'build\runs'
$pidFile = Join-Path $runs "${Name}_pids.txt"
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

if ($Dir -eq '') { $Dir = "t_$Name" }
. (Join-Path $PSScriptRoot 'new-test-dir.ps1')

$relayLog = Join-Path $runs "${Name}_relay.log"
# Memoire du serveur : un fichier par essai, garde d'un lancement a l'autre du meme nom.
$stateFile = Join-Path $runs "${Name}_state.json"
$relay = Start-Process $python -ArgumentList ('-u "' + (Join-Path $project 'tools\relay.py') + '" --log "' + $relayLog + '" --state "' + $stateFile + '"') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if ($relay.HasExited) { Write-Output 'LE RELAIS NE DEMARRE PAS (port 47800 deja pris ?)'; exit 1 }

$ids = @($relay.Id)
$gamePids = @()
$n = 0
foreach ($d in @($Dir, $Dir2)) {
    if ($d -eq '') { continue }
    $n++
    $dataDir = Join-Path $project "build\ryu\$d"
    if (-not (Test-Path $dataDir)) {
        if ($From -ne '') { $dataDir = New-CoopTestDirFromSave -Project $project -Name $d -From $From }
        else { $dataDir = New-CoopTestDir -Project $project -Name $d }
    }
    $modDir = Join-Path $dataDir 'mods\contents\01007ef00011e000\botw-coop\exefs'
    New-Item -ItemType Directory -Force $modDir | Out-Null
    Copy-Item (Join-Path $project 'build\deploy\subsdk9') (Join-Path $modDir 'subsdk9') -Force
    $suffix = if ($n -eq 1) { '' } else { "$n" }
    $gameOut = Join-Path $runs "${Name}_game$suffix.txt"
    $p = Start-Process -FilePath $ryujinx -ArgumentList ('--root-data-dir "' + $dataDir + '" "' + $game + '"') -PassThru -RedirectStandardOutput $gameOut -RedirectStandardError (Join-Path $runs "${Name}_game$suffix.err.txt")
    $ids += $p.Id
    $gamePids += $p.Id
    if ($Screen -gt 0) { & $python (Join-Path $project 'tools\place_windows.py') $Screen $p.Id | Out-Null }
    Write-Output "joueur $n : PID $($p.Id), donnees $d"
    # Le premier jeu doit se connecter au relais avant le second (identifiants 1 puis 2).
    if ($n -eq 1 -and $Dir2 -ne '') { Start-Sleep -Seconds 12 }
}
if ($Screen -gt 0) { & $python (Join-Path $project 'tools\place_windows.py') $Screen @gamePids }
$ids | Set-Content $pidFile
'{"pid": ' + $gamePids[0] + ', "pids": [' + ($gamePids -join ', ') + ']}' | Set-Content (Join-Path $runs "${Name}_drive.json") -Encoding ascii
Remove-Item (Join-Path $runs "${Name}_pilot.log") -ErrorAction SilentlyContinue -Confirm:$false