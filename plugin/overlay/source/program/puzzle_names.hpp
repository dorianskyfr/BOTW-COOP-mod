#pragma once

#include <cstring>

/*
 * Objets d'enigme sans prefixe DgnObj_ / FldObj_ (NOTES 6.30). Releve des cartes des sanctuaires et des Creatures
 * divines (Pack/Dungeon*.pack et Pack/Remains*.pack de la RomFS 1.9.0) : noms d'objets places dont le premier corps
 * de Actor/Physics/<nom>.bphysics a motion_type Dynamic. Ils etaient ignores par le partage des objets mobiles (6.23)
 * et des objets detruits (6.28), qui ne regardaient que les prefixes.
 * Laisses de cote : cordes (Rope, RopeWire, profil Rope), rubis poses (profil Item), engrenages et roues sur axe
 * (SwitchGear*, SpurGearSmall, MagneGear, SwitchWaterWheel_A, Test_SwitchWind), balancoire BalanceLift : leur position
 * est tenue par une liaison, la forcer pourrait lutter contre elle (non essaye).
 */
namespace coop::puzzle_names {

    /* Objets mobiles dont la position est partagee comme celle des DgnObj_ / FldObj_ a corps dynamique. */
    inline bool IsExtraMovable(const char* name) {
        static constexpr const char* Names[] = {
            "AncientBall", "StoneBall", "IronBall", "RockRollA",
            "KibakoDungeon", "Barrel", "Barrel_CDungeon", "Test_MetalBarrel_A",
            "BarrelBomb", "BarrelBomb_Balloon",
            "ElectricBallGenerator", "ElectricBallNoCarriable", "SwitchElectric0_Dynamic", "ElectricGenerator_Dynamic",
        };
        for (const char* n : Names)
            if (std::strcmp(name, n) == 0)
                return true;
        return std::strncmp(name, "Obj_BoxIron_", 12) == 0; /* caisses de metal (magnetisme) */
    }

    /* Objets qui peuvent etre detruits (casses, brules, explosifs) : leur disparition est partagee comme celle des
       DgnObj_ / FldObj_. */
    inline bool IsExtraDestructible(const char* name) {
        static constexpr const char* Names[] = {
            "BlockCrack4x4x4", "KibakoDungeon", "Barrel", "Barrel_CDungeon", "BarrelBomb", "BarrelBomb_Balloon",
        };
        for (const char* n : Names)
            if (std::strcmp(name, n) == 0)
                return true;
        return false;
    }

}
