<#
Prepare les deux fichiers d'une release pour le launcher (rien n'est publie) :
  build\release\botw-coop-mod.zip   : subsdk9 (plugin, notre code), scripts de generation, relais, VERSION
  build\release\BotwCoopLauncher.exe : le launcher (un seul fichier)

  powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -Version 0.95.0

Le plugin doit avoir ete compile sans option d'essai (plugin\build.sh avec COOP_DEFINES vide).
Aucun fichier du jeu n'entre dans l'archive : le launcher genere tout sur le PC du joueur.
La publication (release GitHub du depot public, etiquette v<Version>) se fait a part, sur accord.
#>
param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version attendue : a.b.c (recu : $Version)" }
$out = Join-Path $project 'build\release'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force (Join-Path $stage 'scripts') | Out-Null

$plugin = Join-Path $project 'build\deploy\subsdk9'
if (-not (Test-Path $plugin)) { throw 'Pas de plugin compile (build\deploy\subsdk9)' }
Copy-Item $plugin (Join-Path $stage 'subsdk9')
foreach ($s in 'gamedata_names.py', 'build_sync_flags.py', 'build_travel_points.py', 'build_puppet.py', 'build_item_names.py', 'relay.py') {
    Copy-Item (Join-Path $project "tools\$s") (Join-Path $stage "scripts\$s")
}
[IO.File]::WriteAllText((Join-Path $stage 'VERSION'), "v$Version", (New-Object Text.UTF8Encoding($false)))

$zip = Join-Path $out 'botw-coop-mod.zip'
if (Test-Path $zip) { Remove-Item $zip -Confirm:$false }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)

$dotnet = Join-Path $project 'build\tools\dotnet\dotnet.exe'
& $dotnet publish (Join-Path $project 'tools\launcher\BotwCoopLauncher.csproj') -c Release -o (Join-Path $project 'build\launcher') "-p:Version=$Version" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Compilation du launcher en echec' }
Copy-Item (Join-Path $project 'build\launcher\BotwCoopLauncher.exe') (Join-Path $out 'BotwCoopLauncher.exe') -Force

Get-ChildItem $out -File | ForEach-Object { '{0,-24} {1,12:N0} octets  SHA256 {2}' -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
