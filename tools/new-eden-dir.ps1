<#
Dossiers de test Eden (emulateur derive de yuzu), equivalent de new-test-dir.ps1 pour Ryujinx.

Eden est « portable » quand un dossier user\ est a cote de eden.exe (constate le 2026-11-16 :
eden.exe v0.2.1 cree alors config\, nand\, load\... dans user\ et ne touche plus %APPDATA%\eden).
Chaque dossier de test est donc une copie d'Eden avec son propre user\ :
  build\eden\<Name>\eden.exe ...           liens durs vers le modele (pas de copie)
  build\eden\<Name>\user\keys\              copie
  build\eden\<Name>\user\nand\system\Contents  copie (firmware ; Eden verrouille ces fichiers)
  build\eden\<Name>\user\nand\user\Contents    copie (mise a jour 1.9.0, DLC)
  build\eden\<Name>\user\nand\system\save\  copie (profil utilisateur)
  build\eden\<Name>\user\config\qt-config.ini  copie

Le modele (build\eden\_modele) est fait une fois depuis une installation d'Eden deja reglee
(cles, firmware, profil) : New-EdenModel -Project <racine> -From <dossier d'Eden>.

Usage (dot-source) :
  . tools\new-eden-dir.ps1
  $dir = New-EdenTestDir -Project <racine> -Name e1
#>
function New-EdenModel {
    param([string]$Project, [string]$From)
    $model = Join-Path $Project 'build\eden\_modele'
    if (Test-Path $model) { throw "Le modele existe deja : $model" }
    if (-not (Test-Path (Join-Path $From 'user\keys\prod.keys'))) { throw "Pas de cles dans $From\user\keys" }
    New-Item -ItemType Directory -Force $model | Out-Null
    Get-ChildItem $From -File | Copy-Item -Destination $model
    foreach ($sub in 'keys', 'nand\system\Contents', 'nand\system\save', 'config') {
        $src = Join-Path $From "user\$sub"
        if (Test-Path $src) {
            $dst = Join-Path $model "user\$sub"
            New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
            Copy-Item $src $dst -Recurse
        }
    }
    return $model
}

<#
Recopie la sauvegarde de BOTW d'un autre dossier de test dans un dossier Eden.
  -From : dossier Ryujinx (build\ryu\<From>, sauvegarde bis\user\save\0000000000000001\0)
          ou dossier Eden (build\eden\<From>).
Chemin Eden : user\nand\user\save\0000000000000000\<profil>\01007EF00011E000\, ou <profil> est
l'identifiant du premier profil de profiles.dat (octets 0x10 a 0x1f, ecrits a l'envers : constate
sur l'Eden de l'utilisateur, dossier 0902CF08... pour les octets CB B6 C9 81 ...).
#>
function Get-EdenProfileFolder {
    param([string]$Dir)
    $b = [IO.File]::ReadAllBytes((Join-Path $Dir 'user\nand\system\save\8000000000000010\su\avators\profiles.dat'))
    return (($b[0x1f..0x10] | ForEach-Object { $_.ToString('X2') }) -join '')
}

function Import-EdenSave {
    param([string]$Project, [string]$Dir, [string]$From)
    $ryu = Join-Path $Project "build\ryu\$From\bis\user\save\0000000000000001\0"
    $eden = Join-Path $Project "build\eden\$From"
    if (Test-Path $ryu) { $src = $ryu }
    elseif (Test-Path $eden) {
        $src = Join-Path $eden ('user\nand\user\save\0000000000000000\' + (Get-EdenProfileFolder $eden) + '\01007EF00011E000')
    }
    else { throw "Sauvegarde introuvable pour $From" }
    $dst = Join-Path $Dir ('user\nand\user\save\0000000000000000\' + (Get-EdenProfileFolder $Dir) + '\01007EF00011E000')
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $src '*') $dst -Recurse
}

function New-EdenTestDir {
    param([string]$Project, [string]$Name)
    $model = Join-Path $Project 'build\eden\_modele'
    $dir = Join-Path $Project "build\eden\$Name"
    if (Test-Path $dir) { throw "Le dossier existe deja : $dir (choisir un autre nom de test)" }
    if (-not (Test-Path (Join-Path $model 'eden.exe'))) { throw "Modele absent : $model (New-EdenModel)" }
    New-Item -ItemType Directory -Force (Join-Path $dir 'user\nand\system') | Out-Null
    foreach ($f in Get-ChildItem $model -File) {
        New-Item -ItemType HardLink -Path (Join-Path $dir $f.Name) -Target $f.FullName | Out-Null
    }
    Copy-Item (Join-Path $model 'user\keys') (Join-Path $dir 'user\keys') -Recurse
    Copy-Item (Join-Path $model 'user\config') (Join-Path $dir 'user\config') -Recurse
    # Le qt-config.ini du modele garde les chemins absolus de l'Eden d'origine (nand_directory,
    # load_directory...) : sans correction, le dossier de test ecrirait dans l'Eden de l'utilisateur
    # (constate a l'essai e1 du 2026-11-16). Chaque chemin est ramene dans le user\ du dossier de test.
    $user = (Join-Path (Resolve-Path $dir).Path 'user').Replace('\', '/')
    $cfg = Join-Path $dir 'user\config\qt-config.ini'
    $lines = foreach ($l in [IO.File]::ReadAllLines($cfg)) {
        if ($l -match '^(nand|sdmc|load|dump|tas)_directory=') { "$($Matches[1])_directory=$user/$($Matches[1])" }
        elseif ($l -match '^Screenshots\\screenshot_path=') { "Screenshots\screenshot_path=$user/screenshots" }
        else { $l }
    }
    [IO.File]::WriteAllLines($cfg, $lines, (New-Object Text.UTF8Encoding($false)))
    $save = Join-Path $model 'user\nand\system\save'
    if (Test-Path $save) { Copy-Item $save (Join-Path $dir 'user\nand\system\save') -Recurse }
    # Firmware, mise a jour 1.9.0 et DLC (installes dans le modele par tools\nsp_extract.py ; sans eux,
    # Eden lance avec -g dans un dossier neuf demarre le programme 1.0.0, essai e3) : COPIES, pas de
    # jonction ni de lien dur. Eden ouvre ces fichiers sans partage : une seconde fenetre qui les lisait
    # au meme endroit n'a pas pu les ouvrir et a demarre en 1.0.0 (essai ed1).
    Copy-Item (Join-Path $model 'user\nand\system\Contents') (Join-Path $dir 'user\nand\system\Contents') -Recurse
    $userContents = Join-Path $model 'user\nand\user\Contents'
    if (Test-Path $userContents) {
        New-Item -ItemType Directory -Force (Join-Path $dir 'user\nand\user') | Out-Null
        Copy-Item $userContents (Join-Path $dir 'user\nand\user\Contents') -Recurse
    }
    return $dir
}
