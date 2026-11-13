<#
Test pilote : relais + une instance du jeu (dossier de donnees neuf) + le pilote
(tools/pilot.py) qui dirige le joueur par le reseau. Arrete tout a la fin du
plan ou au bout de -Seconds.

Le plugin est compile avec -DCOOP_REMOTE_PAD (manette pilotee par le reseau).

Usage :
  powershell -File tools\run-pilot.ps1 -Plan explore1 -Name pil1 [-Seconds 420] [-NoBuild] [-Defines '...']
Sorties : build\runs\<Name>_pilot.log, <Name>_game.txt, <Name>_relay.log (hors git).
#>
param(
    [string]$Plan = 'explore1',
    [string]$Name = 'pilot',
    [int]$Seconds = 420,
    [switch]$NoBuild,
    [string]$Defines = '-DCOOP_REMOTE_PAD -DCOOP_TEST_UNFREEZE_TIME'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
# Emplacement de Ryujinx : variable d'environnement COOP_RYUJINX, sinon Documents\Emu-switch.
$ryujinx = if ($env:COOP_RYUJINX) { $env:COOP_RYUJINX } else { Join-Path $env:USERPROFILE 'Documents\Emu-switch\Ryujinx.exe' }
$game    = 'F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild\The Legend of Zelda Breath of the Wild [01007EF00011E000][v0].nsp'
$python  = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
$runs    = Join-Path $project 'build\runs'
New-Item -ItemType Directory -Force $runs | Out-Null

if (-not $NoBuild) {
    $env:MSYSTEM = 'MSYS'
    $env:COOP_PROJECT = $project
    $env:COOP_DEFINES = $Defines
    & C:\msys64\usr\bin\bash.exe -lc 'IFS=; P=$(cygpath -u $COOP_PROJECT); bash $P/plugin/build.sh $COOP_PROJECT > /tmp/coop_build.log 2>&1; r=$?; grep -E -A6 error: /tmp/coop_build.log | head -n 40; exit $r'
    $rc = $LASTEXITCODE
    Remove-Item Env:COOP_DEFINES -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Output "BUILD ECHOUE ($rc)"; exit 1 }
}

$relayLog = Join-Path $runs "${Name}_relay.log"
$pilotLog = Join-Path $runs "${Name}_pilot.log"
$gameOut  = Join-Path $runs "${Name}_game.txt"
Remove-Item $relayLog, $pilotLog -ErrorAction SilentlyContinue -Confirm:$false

$relay = Start-Process $python -ArgumentList ('-u "' + (Join-Path $project 'tools\relay.py') + '" --log "' + $relayLog + '"') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if ($relay.HasExited) { Write-Output 'LE RELAIS NE DEMARRE PAS'; exit 1 }

. (Join-Path $PSScriptRoot 'new-test-dir.ps1')
$dataDir = New-CoopTestDir -Project $project -Name "t_$Name"
$modDir = Join-Path $dataDir 'mods\contents\01007ef00011e000\botw-coop\exefs'
New-Item -ItemType Directory -Force $modDir | Out-Null
Copy-Item (Join-Path $project 'build\deploy\subsdk9') (Join-Path $modDir 'subsdk9') -Force

$gameProc = $null
$pilot = $null
try {
    $gameProc = Start-Process -FilePath $ryujinx -ArgumentList ('--root-data-dir "' + $dataDir + '" "' + $game + '"') -PassThru -RedirectStandardOutput $gameOut -RedirectStandardError (Join-Path $runs "${Name}_game.err.txt")
    # Le pilote se connecte apres le jeu : le jeu recoit l'identifiant 1.
    Start-Sleep -Seconds 25
    $pilot = Start-Process $python -ArgumentList ('-u "' + (Join-Path $project 'tools\pilot.py') + '" --plan ' + $Plan + ' --pid ' + $gameProc.Id + ' --log "' + $pilotLog + '"') -PassThru -WindowStyle Hidden -WorkingDirectory (Join-Path $project 'tools')

    $t = 25
    while ($t -lt $Seconds) {
        Start-Sleep -Seconds 5; $t += 5
        $gameProc.Refresh()
        if ($gameProc.HasExited) { Write-Output "LE JEU S'EST ARRETE SEUL a ~$t s"; break }
        if ($pilot.HasExited) { Write-Output "plan termine a ~$t s"; break }
    }
}
finally {
    foreach ($proc in @($pilot, $gameProc, $relay)) {
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Confirm:$false }
    }
}

Write-Output "=== journal du pilote ($pilotLog) ==="
if (Test-Path $pilotLog) { Get-Content $pilotLog -Encoding utf8 }
Write-Output '=== jeu : lignes utiles ==='
Get-Content $gameOut | Where-Object { $_ -match 'Guest stack|Unhandled exception|botw-coop\] (monde :|acteur distant : (CREE|ECHEC))' } | Select-Object -First 8 | ForEach-Object {
    $s = ($_ -replace '[^\x20-\x7E\t]', '?') -replace '^(\d\d:\d\d:\d\d)\.\d+ .*OutputDebugString: ', '$1 '
    $s.Substring(0, [math]::Min(200, $s.Length))
}
