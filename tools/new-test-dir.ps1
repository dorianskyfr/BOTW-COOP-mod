<#
Cree un dossier de donnees Ryujinx de test NEUF (sans sauvegarde de jeu) dans
build\ryu\<Name>, a partir du modele build\ryu\p1 :
  - system\ (cles), Config.json, listes MAJ/DLC de BOTW : copies ;
  - firmware (bis\system\Contents, 325 Mo) : jonction vers le modele, pas de copie.

Pourquoi un dossier neuf par test : quand une sauvegarde existe deja, l'ecran-titre
ne se deroule plus comme le prevoit le script d'entrees (constate le 2026-11-10).

Usage (dot-source) :
  . tools\new-test-dir.ps1
  $dir = New-CoopTestDir -Project <racine> -Name duo3_p1
#>
function New-CoopTestDir {
    param([string]$Project, [string]$Name)

    $template = Join-Path $Project 'build\ryu\p1'
    $dir = Join-Path $Project "build\ryu\$Name"
    if (Test-Path $dir) { throw "Le dossier existe deja : $dir (choisir un autre nom de test)" }
    if (-not (Test-Path (Join-Path $template 'system\prod.keys'))) { throw "Modele absent : $template" }

    New-Item -ItemType Directory -Force (Join-Path $dir 'bis\system'), (Join-Path $dir 'games\01007ef00011e000') | Out-Null
    Copy-Item (Join-Path $template 'system') (Join-Path $dir 'system') -Recurse
    Copy-Item (Join-Path $template 'Config.json') $dir
    Copy-Item (Join-Path $template 'games\01007ef00011e000\*.json') (Join-Path $dir 'games\01007ef00011e000')
    # Reprend les caches de shaders/JIT du modele pour demarrer plus vite (copie, pas de partage).
    $cache = Join-Path $template 'games\01007ef00011e000\cache'
    if (Test-Path $cache) { Copy-Item $cache (Join-Path $dir 'games\01007ef00011e000\cache') -Recurse }
    cmd /c mklink /J (Join-Path $dir 'bis\system\Contents') (Join-Path $template 'bis\system\Contents') | Out-Null
    return $dir
}

<#
Dossier de test neuf qui reprend la sauvegarde de jeu d'un autre dossier de test
(par exemple une partie deja sortie du sanctuaire de depart).
  $dir = New-CoopTestDirFromSave -Project <racine> -Name duo_p1 -From t_lv1
#>
function New-CoopTestDirFromSave {
    param([string]$Project, [string]$Name, [string]$From)

    $src = Join-Path $Project "build\ryu\$From\bis\user"
    if (-not (Test-Path $src)) { throw "Pas de sauvegarde dans : $src" }
    $dir = New-CoopTestDir -Project $Project -Name $Name
    Copy-Item $src (Join-Path $dir 'bis\user') -Recurse
    $meta = Join-Path $Project "build\ryu\$From\bis\system\save"
    if (Test-Path $meta) { Copy-Item $meta (Join-Path $dir 'bis\system\save') -Recurse }
    return $dir
}
