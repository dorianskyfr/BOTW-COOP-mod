#----------------------------- Configuration botw-coop -----------------------------
# Remplace le config.mk d'exlaunch au moment du build (voir plugin/build.sh).

# Chargement comme module supplementaire (subsdk9) dans l'exefs du jeu.
LOAD_KIND := Module

# The Legend of Zelda: Breath of the Wild (Switch).
PROGRAM_ID := 01007ef00011e000

ELF_EXTRACT :=

PYTHON := python3

NPDM_JSON := application.json

# COOP_DEFINES (variable d'environnement) : options de build, ex. -DCOOP_DUMP_MAIN
C_FLAGS := $(COOP_DEFINES)
# GCC 16 (devkitA64 r30) signale un typedef anonyme dans
# source/lib/hook/nx64/hook_impl.cpp d'exlaunch ; exlaunch compile en -Werror.
CXX_FLAGS := -Wno-non-c-typedef-for-linkage

# Non utilises (pas de deploiement automatique : l'installation est faite a la main).
MOUNT_PATH := /nonexistent
FTP_IP := 127.0.0.1
FTP_PORT := 5000
FTP_USERNAME := anonymous
FTP_PASSWORD :=
RYU_PATH := /nonexistent
