<#
Prepare les fichiers d'une release pour le launcher (rien n'est publie) :
  build\release\botw-coop-mod.zip    : subsdk9 (plugin, notre code), scripts de generation, relais, VERSION
  build\release\BotwCoopLauncher.exe : le launcher (un seul fichier)
  build\release\launcher.json        : {"version": "a.b.c"}, version de cet executable

  powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -Version 0.96.0

Version du launcher : elle ne change que si ses sources ont change depuis la derniere release
(empreinte gardee dans tools\launcher\RELEASED : "<version> <empreinte>"). Sinon l'executable est
recompile avec l'ancienne version et le launcher des joueurs ne se retelecharge pas (il compare
launcher.json a sa propre version). -NewLauncher force une nouvelle version.

Le plugin doit avoir ete compile sans option d'essai (plugin\build.sh avec COOP_DEFINES vide).
Aucun fichier du jeu n'entre dans l'archive : le launcher genere tout sur le PC du joueur.
La publication (release GitHub du depot public, etiquette v<Version>, les trois fichiers) se fait
a part, sur accord.
#>
param([Parameter(Mandatory = $true)][string]$Version, [switch]$NewLauncher)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version attendue : a.b.c (recu : $Version)" }
$out = Join-Path $project 'build\release'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force (Join-Path $stage 'scripts') | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)

$plugin = Join-Path $project 'build\deploy\subsdk9'
if (-not (Test-Path $plugin)) { throw 'Pas de plugin compile (build\deploy\subsdk9)' }
Copy-Item $plugin (Join-Path $stage 'subsdk9')
foreach ($s in 'gamedata_names.py', 'build_sync_flags.py', 'build_travel_points.py', 'build_puppet.py', 'build_item_names.py', 'relay.py') {
    Copy-Item (Join-Path $project "tools\$s") (Join-Path $stage "scripts\$s")
}
[IO.File]::WriteAllText((Join-Path $stage 'VERSION'), "v$Version", $utf8)

$zip = Join-Path $out 'botw-coop-mod.zip'
if (Test-Path $zip) { Remove-Item $zip -Confirm:$false }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)

# Empreinte des sources du launcher (fichiers suivis par git, fins de ligne normalisees)
$launcherDir = Join-Path $project 'tools\launcher'
$sha = [Security.Cryptography.SHA256]::Create()
$all = New-Object Text.StringBuilder
Get-ChildItem $launcherDir -File | Where-Object { $_.Extension -in '.cs', '.csproj', '.config' } | Sort-Object Name | ForEach-Object {
    $text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n"
    $h = [BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($text))) -replace '-', ''
    [void]$all.Append("$($_.Name):$h|")
}
$hash = ([BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($all.ToString()))) -replace '-', '').Substring(0, 16)
$releasedFile = Join-Path $launcherDir 'RELEASED'
$launcherVersion = $Version
if (-not $NewLauncher -and (Test-Path $releasedFile)) {
    $prev = ([IO.File]::ReadAllText($releasedFile)).Trim() -split '\s+'
    if ($prev.Count -eq 2 -and $prev[1] -eq $hash) { $launcherVersion = $prev[0] }
}
if ($launcherVersion -eq $Version) {
    [IO.File]::WriteAllText($releasedFile, "$Version $hash`n", $utf8)
    "Launcher modifie : nouvelle version $Version (tools\launcher\RELEASED mis a jour, a commiter)"
} else {
    "Launcher inchange : il garde la version $launcherVersion (les joueurs ne le retelechargent pas)"
}

$dotnet = Join-Path $project 'build\tools\dotnet\dotnet.exe'
& $dotnet publish (Join-Path $launcherDir 'BotwCoopLauncher.csproj') -c Release -o (Join-Path $project 'build\launcher') "-p:Version=$launcherVersion" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Compilation du launcher en echec' }
Copy-Item (Join-Path $project 'build\launcher\BotwCoopLauncher.exe') (Join-Path $out 'BotwCoopLauncher.exe') -Force
[IO.File]::WriteAllText((Join-Path $out 'launcher.json'), "{`"version`": `"$launcherVersion`"}`n", $utf8)

Get-ChildItem $out -File | ForEach-Object { '{0,-24} {1,12:N0} octets  SHA256 {2}' -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
