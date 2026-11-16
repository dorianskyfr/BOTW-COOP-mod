"""Affiche le contenu d'un paquet d'acteur du jeu (outil d'essai, lecture seule).

Un .sbactorpack (Yaz0 + SARC) contient les reglages de l'acteur (fichiers AAMP : ActorLink .bxml,
IA .baiprog, parametres .bgparamlist...). On les affiche en texte (oead), pour lire par exemple les
drapeaux qu'une IA consulte. Le paquet peut etre un fichier ou une entree d'un autre paquet SARC.

Usage : python -I tools\\dump_actorpack.py <fichier.sbactorpack>
        python -I tools\\dump_actorpack.py <paquet.pack> <Actor/Pack/Nom.sbactorpack>
"""
import sys

import oead


def main(path, inner=None):
    sys.stdout.reconfigure(encoding="utf-8")  # noms japonais dans les fichiers d'IA
    raw = open(path, "rb").read()
    if inner is not None:
        raw = bytes(oead.Sarc(raw).get_file(inner).data)
    if raw[:4] == b"Yaz0":
        raw = oead.yaz0.decompress(raw)
    for f in oead.Sarc(raw).get_files():
        data = bytes(f.data)
        print(f"===== {f.name} ({len(data)} octets)")
        if data[:4] == b"AAMP":
            print(oead.aamp.ParameterIO.from_binary(data).to_text())
        elif data[:2] in (b"BY", b"YB"):
            print(oead.byml.to_text(oead.byml.from_binary(data)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
