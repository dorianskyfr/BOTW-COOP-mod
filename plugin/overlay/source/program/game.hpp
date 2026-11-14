#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Acces aux donnees du jeu. Tout ce qui depend d'une adresse propre a une
 * version de BOTW passe par ici, et n'est actif que si le build ID du module
 * main correspond a une version dont les adresses ont ete verifiees.
 */
namespace coop::game {

    /* Lit le build ID de main, le journalise, et decide si la version est connue. */
    void Init();

    /*
     * A appeler au debut de chaque image (fil du jeu) : renouvelle periodiquement le
     * cache des plages memoire lisibles utilise par les lectures protegees.
     */
    void OnFrameStart();

    /* true si [addr, addr+size) est mappe en lecture (avec cache, voir game.cpp). */
    bool CanReadMemory(std::uintptr_t addr, std::size_t size);

    /* Nombre de lectures refusees parce que l'adresse n'etait pas mappee en lecture. */
    std::uint64_t RefusedReads();

    /* Nombre total d'appels a svcQueryMemory faits par les lectures protegees (mesure). */
    std::uint64_t MemoryQueries();

    /* true si le build ID correspond a une version dont on connait les adresses. */
    bool IsSupported();

    struct PlayerState {
        std::uintptr_t m_PlayerInfo = 0;  /* uking::act::PlayerInfo* (0 si pas encore cree) */
        std::uintptr_t m_PlayerActor = 0; /* PlayerInfo::mPlayerActor (0 hors jeu) */
        float m_Pos[3] = { 0, 0, 0 };     /* PlayerInfo::mPlayerPos */
    };

    /* false si la version n'est pas supportee ou si PlayerInfo n'existe pas encore. */
    bool ReadPlayer(PlayerState& out);

    /*
     * « Monde pret » : le joueur est en jeu et son acteur est actif dans la liste des
     * acteurs depuis ~1 s. Faux a l'ecran-titre et pendant tout chargement (y compris
     * apres un Game Over). Calcule par le fil du jeu, lisible de tout fil.
     */
    bool IsWorldReady();

    /* ---- Monde : heure, jours, meteo ---- */

    struct WorldState {
        std::uintptr_t m_WorldMgr = 0;   /* ksys::world::Manager* */
        std::uintptr_t m_TimeMgr = 0;    /* ksys::world::TimeMgr* (mMgrs[0]) */
        int   m_NumMgrs = 0;             /* taille de Manager::mMgrs, pour controle */
        float m_Time = 0;                /* TimeMgr::mTime : 0..360, 15 par heure */
        float m_TimeStep = 0;            /* TimeMgr::mTimeStep : avance par image */
        float m_BloodMoonTimer = 0;      /* TimeMgr::mBloodMoonTimer */
        int   m_NumberOfDays = 0;        /* TimeMgr::mNumberOfDays */
        int   m_Weather = -1;            /* WeatherMgr::mWeather (octet) */
        int   m_Climate = -1;            /* Manager+0x780 : climat courant (-1 si illisible) */
    };

    /* false si la version n'est pas supportee ou si le gestionnaire du monde n'existe pas. */
    bool ReadWorld(WorldState& out);

    /*
     * Demande au jeu de regler l'heure : ecrit TimeMgr::mNewTime, que le jeu
     * applique lui-meme a l'image suivante (worldTimeMgr.cpp, calc). 0 <= time < 360.
     */
    bool RequestWorldTime(float time);

    /* Demande de meteo en cours dans ksys::world::Manager (mWeatherType, mWeatherTypeTimer, mWeatherSetForDemo). */
    bool ReadWeatherRequest(int& type, int& timer, int& forDemo);
    /* Demande une meteo (0 Bluesky ... 8 BlueskyRain), a renouveler : le jeu l'oublie en 4 images. */
    bool RequestWeather(int type);

    /*
     * OUTIL DE TEST UNIQUEMENT : force TimeMgr::mPlayedDemo103Or997 a vrai en
     * memoire, pour que l'heure s'ecoule avant la sortie du sanctuaire de depart
     * (sinon le jeu la remet a 5 h 15 a chaque image). Ne touche pas aux flags de
     * sauvegarde. Jamais dans un build destine a jouer.
     */
    bool DebugUnfreezeTime();

    /* ---- Acteurs ---- */

    /*
     * Poignee de creation d'acteur : meme disposition que ksys::act::BaseProcHandle
     * (decomp 1.5.0, actBaseProcHandle.h) : pointeur vers l'unite + drapeau d'echec.
     * Doit rester en memoire statique tant que l'acteur existe.
     */
    struct ActorHandle {
        std::uintptr_t m_Unit   = 0;
        bool           m_Failed = false;
    };
    static_assert(sizeof(ActorHandle) == 0x10);

    /*
     * Demande au jeu de creer un acteur (asynchrone), a la position donnee.
     * A appeler depuis le fil principal du jeu. false si refuse ou non supporte.
     */
    bool RequestCreateActor(const char* name, const float pos[3], ActorHandle& handle);

    /*
     * Meme demande, sans poignee. Avec une poignee, le jeu laisse l'acteur endormi
     * apres son initialisation (decomp actBaseProc.cpp, finalizeInit_ : sleep si
     * mProcUnit, sinon wakeUp) ; sans poignee il est reveille, donc anime et dessine.
     * L'acteur se retrouve ensuite avec FindActor().
     */
    bool RequestCreateActorAwake(const char* name, const float pos[3]);

    /*
     * Cherche dans la liste d'ActorCreator un acteur de ce nom a moins de `radius`
     * metres de `pos`, different de `exclude`. 0 si aucun.
     */
    std::uintptr_t FindActor(const char* name, const float pos[3], float radius, std::uintptr_t exclude = 0);

    /* Un acteur de la liste d'ActorCreator, tel que vu par ForEachListedActor. */
    struct ActorInfo {
        std::uintptr_t m_Actor;
        std::uint32_t  m_HashId;   /* Actor::mHashId : identifiant d'objet de carte, 0 si cree en jeu */
        const char*    m_Name;     /* BaseProc::mName */
        float          m_Pos[3];
    };

    /* Parcourt la liste d'ActorCreator (fil du jeu). Le rappel renvoie true pour arreter. */
    void ForEachListedActor(bool (*fn)(const ActorInfo& info, void* user), void* user);

    /*
     * Demande au jeu de detruire un acteur (BaseProc::deleteLater, raison 0).
     * false si la version n'est pas supportee ou si le jeu refuse (deja en destruction).
     */
    bool DeleteActor(std::uintptr_t actor);

    /* true si ce pointeur figure encore dans la liste d'ActorCreator (acteur vivant). */
    bool IsActorListed(std::uintptr_t actor);

    /* Nombre d'acteurs de la liste d'ActorCreator (mesure). */
    int ListedActorCount();

    /* BaseProc::mState (0 Init, 1 Calc, 2 Sleep, 3 Delete), -1 si inconnu. */
    int GetActorState(std::uintptr_t actor);

    /* Acteur associe a la poignee une fois sa creation terminee, sinon 0. */
    std::uintptr_t GetReadyActor(const ActorHandle& handle);

    /* Etat de l'unite de creation (ksys::act::BaseProcUnit::Status), -1 si pas d'unite. */
    int GetHandleStatus(const ActorHandle& handle);

    /* Lecture / ecriture de la translation de la matrice d'un acteur (Actor::mMtx). */
    void GetActorPos(std::uintptr_t actor, float out[3]);
    void SetActorPos(std::uintptr_t actor, const float pos[3]);

    /* Matrice complete (sead::Matrix34f : 3 lignes de 4 floats, rotation + translation). */
    void GetActorMtx(std::uintptr_t actor, float out[12]);
    void SetActorMtx(std::uintptr_t actor, const float mtx[12]);

    /*
     * Place le corps physique principal de l'acteur (teleportation par le moteur
     * physique du jeu). 0 si fait, negatif si l'acteur n'a pas de corps exploitable
     * (-1 pas de corps, -2 pas d'accesseur, -3/-4/-5 controle de coherence echoue).
     */
    int SetActorBodyTransform(std::uintptr_t actor, const float mtx[12]);

    /* Place un acteur a controleur de personnage (PNJ). 0 si fait, negatif sinon. */
    /* Renvoie le nombre de corps physiques du controleur deplaces (0 = aucun trouve). */
    /* Ecrit TimeMgr::mBloodMoonTimer (+0xc8). Seul, sans effet quand le jeu est en pause
       (essai k8) ; a utiliser avec l'ecriture du drapeau WM_BloodyMoonTimer. Fil du jeu. */
    bool SetBloodMoonTimerField(float value);

    int SetActorCharacterPos(std::uintptr_t actor, const float mtx[12], bool log);

    /* Diagnostic : journalise ou la position courante de l'acteur est stockee (acteur et objets pointes). */
    void LogPositionSites(std::uintptr_t actor);

    /* Diagnostic : journalise l'objet Actor::mAttention de l'acteur et les codes d'action qu'il reference. */
    void LogAttention(std::uintptr_t actor);

    /*
     * Coupe les interactions proposees par un acteur (invite « Parler », verrouillage...).
     * Renvoie le nombre d'entrees retirees (0 si deja fait), negatif si l'acteur n'a pas
     * d'objet d'attention ou si sa forme n'est pas celle attendue (rien n'est ecrit).
     */
    int DisableActorAttention(std::uintptr_t actor);

    /* Vie du joueur local en quarts de cœur (et maximum), lifePtr : ou elle est rangee. Fil du jeu. */
    bool GetPlayerLife(int& life, int& maxLife, std::uintptr_t& lifePtr);

    /* Valeur de getLife (case 87) d'un acteur ; pour une arme, sa durabilite. Fil du jeu. */
    bool GetActorLifeValue(std::uintptr_t actor, int& value);

    /* Bonus d'une arme (WeaponModifierInfo du jeu) : bits WeaponModifier, valeur. flags = 0 : aucun. */
    struct WeaponBonus {
        std::uint32_t m_Flags = 0;
        std::int32_t  m_Value = 0;
    };

    /* Ajoute un objet a l'inventaire du joueur (itemGet ; pour une arme, value = durabilite, bonus
       facultatif). before / after : nombre d'entrees de la liste d'objets avant et apres. Fil du jeu. */
    bool AddInventoryItem(const char* name, int value, int& before, int& after, const WeaponBonus* bonus = nullptr);

    /* Un objet de l'inventaire (PouchItem du jeu). Type : 0 epee, 1 arc, 2 fleche, 3 bouclier,
       4-6 armure, 7 materiau, 8 plat, 9 objet cle. */
    struct PouchEntry {
        char          m_Name[48] = {};
        int           m_Type = -1;
        int           m_Value = 0;   /* durabilite pour une arme, nombre pour une pile */
        bool          m_Equipped = false;
        WeaponBonus   m_Bonus;
    };

    /* Lit l'inventaire (au plus max objets, filtre facultatif par types : bit 1 << type).
       Renvoie le nombre d'objets lus, -1 si l'inventaire est illisible. Fil du jeu. */
    int ReadPouch(PouchEntry* out, int max, std::uint32_t typeMask = 0xffffffffu);

    /* Voyage du jeu (changement de lieu avec chargement) : carte "MainField/A-1", point "StartPos_CDungeon118".
       Le jeu peut l'ignorer pendant les ~20 s qui suivent un chargement. Fil du jeu. */
    bool ChangeStage(const char* map, const char* startPos);

    /* ---- Carte : tampons (marqueurs poses sur la carte) ---- */

    struct MapStamp {
        std::uintptr_t m_Object = 0;
        int            m_Icon = -1;
        float          m_Pos[3] = {};
        std::uint32_t  m_Flags = 0;   /* 0x10 actif, 0x8 pose, 0x200 visible */
    };

    /* Nombre de tampons du gestionnaire de carte (reserve du jeu), -1 si illisible. Fil du jeu. */
    int MapStampCount();
    /* Tampon n.index de la reserve ; false si illisible. */
    bool ReadMapStamp(int index, MapStamp& out);
    /* Pose un tampon (fonction du jeu qui prend un tampon libre de la reserve). Fil du jeu. */
    bool AddMapStamp(int icon, const float pos[3]);
    /* Deplace / retire (desactive) un tampon de la reserve. Fil du jeu. */
    bool SetMapStampPos(std::uintptr_t stamp, const float pos[3]);
    bool SetMapStampActive(std::uintptr_t stamp, bool active);

    /* Nom d'un acteur (BaseProc::mName), ou "" si le pointeur de chaine est invalide. */
    const char* GetActorName(std::uintptr_t actor);

    /*
     * Demarre une sequence d'animation (nom de l'ASList de l'acteur) sur le creneau 0.
     * Le jeu ne dit pas si elle a demarre : false seulement si l'appel n'a pas pu etre fait.
     * Fil du jeu.
     */
    bool PlayActorSequence(std::uintptr_t actor, const char* name);

    /*
     * Diagnostic : cherche dans l'objet acteur du joueur ou se trouvent sa position
     * et son nom, et journalise les decalages trouves. Sert a verifier sur la
     * version courante les decalages tires de la decompilation 1.5.0.
     */
    void LogPlayerActorLayout();

    /*
     * Outil d'analyse : journalise le nom et la position des acteurs situes a
     * moins de `radius` metres du joueur (liste d'ActorCreator). A appeler depuis
     * le fil du jeu. Sert a cartographier les objets interactifs sans voir l'image.
     */
    void LogNearbyActors(float radius, float farRadius = 0.0f);

    /* Outil de test : place le Bokoblin de carte le plus proche au-dessus du joueur (essai chute, NOTES 5.11). */
    int DropNearestEnemy(float height, float ahead);
    /*
     * Points de vie d'un ennemi actif (Actor::getLife / getMaxLife, cases 87 et 30 de la
     * vtable, mesurees a l'essai kw). lifePtr : ou ecrire. false si ce n'est pas un ennemi
     * actif ou si une valeur n'est pas plausible. Fil du jeu.
     */
    bool GetEnemyLife(std::uintptr_t actor, int& life, int& maxLife, std::uintptr_t& lifePtr);
    /* Outil de test : journalise les points de vie des ennemis a moins de `radius` metres. */
    void LogEnemyLife(float radius);
    void DropHoldOnFrame();

}
