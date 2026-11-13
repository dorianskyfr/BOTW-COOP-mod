<#
Test a deux instances : demarre le relais, compile le plugin, l'installe dans
deux dossiers de donnees Ryujinx de test (build\ryu\p1 et p2), lance BOTW dans
deux instances, capture les sorties, mesure la charge, puis arrete tout ce que
ce script a lance.

Usage :
  powershell -File tools\run-duo.ps1 -Seconds 240 -Name duo1 [-NoBuild] [-Defines '-DCOOP_INPUT_SCRIPT']
Sorties : build\runs\<Name>_p1.txt, <Name>_p2.txt, <Name>_relay.log (hors git).
#>
param(
    [int]$Seconds = 240,
    [string]$Name = 'duo',
    [switch]$NoBuild,
    [string]$Defines = '-DCOOP_INPUT_SCRIPT',
    [int]$StaggerSeconds = 20,
    [string]$Pattern = 'botw-coop\] (plugin|build id|coop:|net: (le relais|J\d|relais|client|Connect|symbole)|input: SCRIPT)|Guest stack|Unhandled exception'
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
    & C:\msys64\usr\bin\bash.exe -lc 'IFS=; P=$(cygpath -u $COOP_PROJECT); bash $P/plugin/build.sh $COOP_PROJECT > /tmp/coop_build.log 2>&1; r=$?; grep -E -A6 error: /tmp/coop_build.log | head -n 40; tail -n 1 /tmp/coop_build.log; exit $r'
    $rc = $LASTEXITCODE
    Remove-Item Env:COOP_DEFINES -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Output "BUILD ECHOUE ($rc)"; exit 1 }
}

$relayLog = Join-Path $runs "${Name}_relay.log"
Remove-Item $relayLog -ErrorAction SilentlyContinue -Confirm:$false
# Les chemins contiennent des espaces : chaque argument est mis entre guillemets a la main.
$relayArgs = '-u "' + (Join-Path $project 'tools\relay.py') + '" --log "' + $relayLog + '"'
$relay = Start-Process $python -ArgumentList $relayArgs -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if ($relay.HasExited -or -not (Get-NetTCPConnection -LocalPort 47800 -State Listen -ErrorAction SilentlyContinue)) {
    Write-Output 'LE RELAIS NE DEMARRE PAS, test annule'
    exit 1
}

$procs = @()
try {
    . (Join-Path $PSScriptRoot 'new-test-dir.ps1')
    foreach ($player in 'p1', 'p2') {
        # Un dossier de donnees neuf par joueur et par test (pas de sauvegarde existante).
        $dataDir = New-CoopTestDir -Project $project -Name "t_${Name}_$player"
        $modDir = Join-Path $dataDir 'mods\contents\01007ef00011e000\botw-coop\exefs'
        New-Item -ItemType Directory -Force $modDir | Out-Null
        Copy-Item (Join-Path $project 'build\deploy\subsdk9') (Join-Path $modDir 'subsdk9') -Force

        $out = Join-Path $runs "${Name}_$player.txt"
        $err = Join-Path $runs "${Name}_$player.err.txt"
        $argList = '--root-data-dir "' + $dataDir + '" "' + $game + '"'
        $p = Start-Process -FilePath $ryujinx -ArgumentList $argList -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        $procs += [pscustomobject]@{ Player = $player; Proc = $p; Out = $out }
        Write-Output "$player : PID $($p.Id)"
        if ($player -eq 'p1') { Start-Sleep -Seconds $StaggerSeconds }
    }

    $smi = (Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
    $t = 0
    $sampleAt = [math]::Max(30, $Seconds - 40)
    while ($t -lt $Seconds) {
        Start-Sleep -Seconds 10; $t += 10
        foreach ($e in $procs) { $e.Proc.Refresh() }
        if (($procs | Where-Object { $_.Proc.HasExited }).Count -eq $procs.Count) { break }
        if ($t -eq $sampleAt -or ($t -gt $sampleAt -and $t -lt $sampleAt + 10)) {
            $before = @{}; foreach ($e in $procs) { if (-not $e.Proc.HasExited) { $before[$e.Player] = $e.Proc.TotalProcessorTime.TotalSeconds } }
            Start-Sleep -Seconds 10; $t += 10
            foreach ($e in $procs) {
                $e.Proc.Refresh()
                if (-not $e.Proc.HasExited -and $before.ContainsKey($e.Player)) {
                    $cpu = ($e.Proc.TotalProcessorTime.TotalSeconds - $before[$e.Player]) / 10 * 100 / [Environment]::ProcessorCount
                    Write-Output ("charge {0} a t={1}s : CPU {2:N1} % de la machine, RAM {3:N0} Mo" -f $e.Player, $t, $cpu, ($e.Proc.WorkingSet64 / 1MB))
                }
            }
            if ($smi) { Write-Output ("charge GPU : " + (& $smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader)) }
            Write-Output ("RAM libre systeme : {0:N1} Go" -f ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB))
        }
    }
}
finally {
    foreach ($e in $procs) {
        $e.Proc.Refresh()
        if ($e.Proc.HasExited) { Write-Output "$($e.Player) : INSTANCE TERMINEE SEULE" }
        else { Stop-Process -Id $e.Proc.Id -Confirm:$false; Write-Output "$($e.Player) : arretee apres le test" }
    }
    Start-Sleep -Seconds 2
    if (-not $relay.HasExited) { Stop-Process -Id $relay.Id -Confirm:$false }
}

foreach ($e in $procs) {
    $lines = Get-Content $e.Out
    Write-Output "=== $($e.Player) : $($lines.Count) lignes ==="
    $sel = $lines | Where-Object { $_ -match $Pattern } | ForEach-Object {
        $s = $_ -replace '[^\x20-\x7E\t]', '?'
        $s = $s -replace '^(\d\d:\d\d:\d\d)\.\d+ .*OutputDebugString: ', '$1 '
        $s.Substring(0, [math]::Min(250, $s.Length))
    }
    $sel | Select-Object -First 6
    Write-Output '   ...'
    $sel | Select-Object -Last 14
}
Write-Output '=== relais ==='
Get-Content $relayLog | Select-Object -First 4
Write-Output '   ...'
Get-Content $relayLog | Select-Object -Last 8
