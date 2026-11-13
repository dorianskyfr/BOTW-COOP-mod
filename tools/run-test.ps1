<#
Outil de test : compile le plugin, l'installe dans un dossier de donnees Ryujinx
DE TEST (jamais le dossier reel de l'utilisateur), lance BOTW dans une nouvelle
instance, capture la sortie console, puis arrete CETTE instance.

Pourquoi la console et pas le fichier log : Ryujinx ne garde que 3 logs et
echoue a en creer un nouveau si le plus ancien est verrouille par une autre
instance ouverte (constate le 2026-11-10, voir NOTES.md).

Dossier de test : build\ryu\<Player> (copie des cles, du firmware et de la
config de l'utilisateur, hors git). Passe a Ryujinx par --root-data-dir.

Usage :
  powershell -File tools\run-test.ps1 -Seconds 40 -Name essai1 [-NoBuild] [-Player p1] [-Pattern 'regex'] [-Defines '-DX']
Sortie complete : build\runs\<Name>.txt (hors git).
#>
param(
    [int]$Seconds = 40,
    [string]$Name = 'run',
    [switch]$NoBuild,
    [string]$Player = 'p1',
    # Cree un dossier de donnees neuf (sans sauvegarde) build\ryu\t_<Name> pour ce test.
    [switch]$Fresh,
    # Essais temoins : sans plugin, et/ou sans la mise a jour (jeu de base 1.0.0).
    [switch]$NoPlugin,
    [switch]$NoUpdate,
    [switch]$OpenGL,
    [string]$Defines = '',
    # Instants (en secondes, multiples de 5) ou capturer la fenetre du jeu : '40,60,90'
    [string]$ShotsAt = '',
    [string]$Pattern = 'botw-coop|Guest stack|Unhandled exception|Application Loaded'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
# Emplacement de Ryujinx : variable d'environnement COOP_RYUJINX, sinon Documents\Emu-switch.
$ryujinx = if ($env:COOP_RYUJINX) { $env:COOP_RYUJINX } else { Join-Path $env:USERPROFILE 'Documents\Emu-switch\Ryujinx.exe' }
$game    = 'F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild\The Legend of Zelda Breath of the Wild [01007EF00011E000][v0].nsp'
if ($Fresh) {
    . (Join-Path $PSScriptRoot 'new-test-dir.ps1')
    $Player = "t_$Name"
    [void](New-CoopTestDir -Project $project -Name $Player)
}
$dataDir = Join-Path $project "build\ryu\$Player"
$modDir  = Join-Path $dataDir 'mods\contents\01007ef00011e000\botw-coop\exefs'
$runs    = Join-Path $project 'build\runs'

if (-not (Test-Path (Join-Path $dataDir 'system\prod.keys'))) {
    Write-Output "Dossier de test absent ou incomplet : $dataDir"
    exit 1
}

if (-not $NoBuild) {
    $env:MSYSTEM = 'MSYS'
    $env:COOP_PROJECT = $project
    $env:COOP_DEFINES = $Defines
    # Pas de guillemets doubles dans la commande bash : PowerShell 5.1 les supprime
    # en passant l'argument a un executable natif.
    & C:\msys64\usr\bin\bash.exe -lc 'IFS=; P=$(cygpath -u $COOP_PROJECT); bash $P/plugin/build.sh $COOP_PROJECT > /tmp/coop_build.log 2>&1; r=$?; grep -E -A6 error: /tmp/coop_build.log | head -n 40; tail -n 1 /tmp/coop_build.log; exit $r'
    $rc = $LASTEXITCODE
    Remove-Item Env:COOP_DEFINES -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Output "BUILD ECHOUE ($rc)"; exit 1 }
}

New-Item -ItemType Directory -Force $runs | Out-Null
if (-not $NoPlugin) {
    New-Item -ItemType Directory -Force $modDir | Out-Null
    Copy-Item (Join-Path $project 'build\deploy\subsdk9') (Join-Path $modDir 'subsdk9') -Force
}
if ($OpenGL) {
    # Rendu OpenGL pour ce dossier de test uniquement (les captures d'ecran ne voient pas la surface Vulkan).
    $cfgPath = Join-Path $dataDir 'Config.json'
    $cfgText = [IO.File]::ReadAllText($cfgPath) -replace '"graphics_backend"\s*:\s*"Vulkan"', '"graphics_backend": "OpenGl"'
    [IO.File]::WriteAllText($cfgPath, $cfgText, (New-Object Text.UTF8Encoding($false)))
}
if ($NoUpdate) {
    # Essai temoin sur le jeu de base (1.0.0) : vide la liste des mises a jour du dossier de test.
    $upd = Join-Path $dataDir 'games\01007ef00011e000\updates.json'
    [IO.File]::WriteAllText($upd, '{ "selected": "", "paths": [] }', (New-Object Text.UTF8Encoding($false)))
}

$out = Join-Path $runs "$Name.txt"
$err = Join-Path $runs "$Name.err.txt"
$argList = '--root-data-dir "' + $dataDir + '" "' + $game + '"'
$p = Start-Process -FilePath $ryujinx -ArgumentList $argList -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
Write-Output "PID $($p.Id), donnees $dataDir, observation $Seconds s"

$shotTimes = @()
if ($ShotsAt) { $shotTimes = $ShotsAt.Split(',') | ForEach-Object { [int]$_ } }
if ($shotTimes.Count -gt 0) { . (Join-Path $PSScriptRoot 'capture-window.ps1') }

$t = 0
while ($t -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds 5; $t += 5; $p.Refresh()
    if ($shotTimes -contains $t -and -not $p.HasExited) {
        $png = Join-Path $runs ("{0}_t{1:000}.png" -f $Name, $t)
        # (La capture integree de Ryujinx, F8, echoue sur BOTW 1.9.0 : nom de fichier invalide.)
        # Capture par duplication de bureau (tools/capture_dd.py) : la seule methode essayee
        # qui peut voir l'image du jeu ; GDI et PrintWindow donnent du noir.
        $pyExe = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
        $shot = & $pyExe -I (Join-Path $PSScriptRoot 'capture_dd.py') $p.Id $png 2>&1 | Select-Object -Last 1
        Write-Output "capture t=$t s : $shot"
    }
}

if ($p.HasExited) {
    Write-Output "INSTANCE TERMINEE SEULE apres ~$t s"
} else {
    Write-Output ("vivante apres $t s : CPU {0} s, RAM {1} Mo" -f [int]$p.CPU, [int]($p.WorkingSet64 / 1MB))
    Stop-Process -Id $p.Id -Confirm:$false
}

$lines = Get-Content $out
Write-Output "$($lines.Count) lignes -> $out"
$lines | Where-Object { $_ -match $Pattern } | ForEach-Object {
    $s = $_ -replace '[^\x20-\x7E\t]', '?'
    $s = $s -replace '^(\d\d:\d\d:\d\d)\.\d+ .*OutputDebugString: ', '$1 '
    $s.Substring(0, [math]::Min(230, $s.Length))
}
