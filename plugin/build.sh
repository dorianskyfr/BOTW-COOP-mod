#!/bin/bash
# Build du plugin botw-coop avec exlaunch + devkitA64.
#
# Usage (depuis un shell MSYS2) :
#   bash plugin/build.sh "<chemin du projet>"
#
# Le Makefile d'exlaunch ne supporte pas les espaces dans les chemins, et le
# dossier du projet en contient. On copie donc exlaunch + notre overlay dans un
# dossier de travail sans espace ($HOME/botwcoop-build), on compile la, puis on
# recopie le resultat dans <projet>/build/deploy.
set -euo pipefail

PROJECT="${1:?chemin du projet requis}"
PROJECT="$(cygpath -u "$PROJECT")"
# COOP_WORK : dossier de travail (un par variante de build, les objets ne sont pas compatibles).
# COOP_MAKE_ARGS : arguments make supplementaires, ex. "LOAD_KIND=AsRtld".
WORK="${COOP_WORK:-$HOME/botwcoop-build}"
TITLE_ID="01007ef00011e000"

export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

mkdir -p "$WORK"
# Copie d'exlaunch (sans son .git), puis overlay de nos fichiers par-dessus.
cp -r "$PROJECT/third_party/exlaunch/." "$WORK/"
rm -rf "$WORK/.git"
cp -r "$PROJECT/plugin/overlay/." "$WORK/"

# Le .npdm est genere depuis config.json : on y met le Title ID de BOTW.
sed -i -E "s/\"title_id\": *\"0x[0-9a-fA-F]+\"/\"title_id\": \"0x${TITLE_ID}\"/" "$WORK/config.json"
# npdmtool (switch-tools 1.13) attend les champs "program_id*" et non "title_id*"
# (constate au build : "Failed to get program_id (field not present)").
sed -i -E 's/"title_id/"program_id/g' "$WORK/config.json"
grep '"program_id' "$WORK/config.json"

# En-tete de module attendu par le rtld de BOTW 1.9.0.
# Constat (NOTES 1 bis) : avec l'en-tete d'origine d'exlaunch, le premier mot du
# module est "b entrypoint" = 0x14000008 ; le rtld 1.9.0 plante avec X8=0x14000008
# et X9=0x14000002 ("b +8"). On fait donc commencer le module par "b +8", le mot
# suivant reste l'offset MOD0, et le saut vers entrypoint est place a +8.
# Desactivable avec COOP_PATCH_HEADER=0.
if [ "${COOP_PATCH_HEADER:-1}" = "1" ]; then
  CRT0="$WORK/source/lib/init/crt0.s"
  sed -i -E \
    -e '/^__module_start:/,/^entrypoint:/ s/^([[:space:]]+)b entrypoint[[:space:]]*$/\1b __coop_header_tail/' \
    -e '/^__module_start:/,/^entrypoint:/ s/^([[:space:]]+)(\.word __nx_mod0 - __module_start)[[:space:]]*$/\1\2\n__coop_header_tail:\n\1b entrypoint/' \
    "$CRT0"
  grep -q '__coop_header_tail:' "$CRT0" || { echo "patch d'en-tete non applique" >&2; exit 1; }
  sed -n '/^__module_start:/,/^entrypoint:/p' "$CRT0"
fi

cd "$WORK"
make -j8 ${COOP_MAKE_ARGS:-}

OUT_DIR="$PROJECT/build/${COOP_OUT:-deploy}"
mkdir -p "$OUT_DIR"
cp -f "$WORK/deploy/"* "$OUT_DIR/"
ls -la "$OUT_DIR"
