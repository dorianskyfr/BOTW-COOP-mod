#include "champion_fx.hpp"

#include "clock.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

namespace coop::champion_fx {

    namespace {
        /* 1.9.0 : Actor ELink (main+0x1100bd0) lit l'utilisateur +0x48 dans
           le composant +0x568. La création ELink (f50e80) ne joue que les particules. */
        constexpr std::uintptr_t CreateEffect = 0xf50e80;
        constexpr const char* Names[Count] = { nullptr, "Kago_Wind", "Kago_Lightning" };
        std::atomic<std::uintptr_t> s_LocalUser { 0 };

        struct Handle {
            std::uintptr_t m_Effect = 0;
            std::uint32_t m_Id = 0;
        };

        std::uintptr_t EffectUser(std::uintptr_t actor) {
            if (actor == 0 || !coop::game::CanReadMemory(actor + 0x568, sizeof(std::uintptr_t)))
                return 0;
            const auto links = *reinterpret_cast<const std::uintptr_t*>(actor + 0x568);
            if (links == 0 || !coop::game::CanReadMemory(links + 0x48, sizeof(std::uintptr_t)))
                return 0;
            return *reinterpret_cast<const std::uintptr_t*>(links + 0x48);
        }

        HOOK_DEFINE_TRAMPOLINE(CreateEffectHook) {
            static void Callback(std::uintptr_t user, const char* name, Handle* handle) {
                Orig(user, name, handle);
                if (user == 0 || user != s_LocalUser.load(std::memory_order_relaxed) ||
                    name == nullptr || name[0] != 'K' || handle == nullptr || handle->m_Effect == 0)
                    return;
                for (unsigned effect = Wind; effect < Count; effect++) {
                    if (std::strcmp(name, Names[effect]) == 0) {
                        g_Local[effect].store(coop::clock::NowMs(), std::memory_order_relaxed);
                        return;
                    }
                }
            }
        };
    }

    void Install() {
        CreateEffectHook::InstallAtOffset(CreateEffect);
    }

    void SetLocalActor(std::uintptr_t actor) {
        s_LocalUser.store(EffectUser(actor), std::memory_order_relaxed);
    }

    void PlayPending(std::uintptr_t puppet) {
        for (unsigned effect = Wind; effect < Count; effect++) {
            const auto received = g_Remote[effect].exchange(0, std::memory_order_relaxed);
            if (received == 0 || coop::clock::NowMs() - received > MaxAgeMs)
                continue;
            if (const auto user = EffectUser(puppet); user != 0) {
                Handle handle;
                CreateEffectHook::Orig(user, Names[effect], &handle);
                if (handle.m_Effect != 0)
                    Logging.Log("[botw-coop] Prodige : %s affiche pour le partenaire", Names[effect]);
            }
        }
    }

    void ClearRemote() {
        for (auto& effect : g_Remote)
            effect.store(0, std::memory_order_relaxed);
    }

#ifdef COOP_TEST_CHAMPION_FX
    void DebugEmit(Effect effect) {
        const auto user = s_LocalUser.load(std::memory_order_relaxed);
        if (user != 0) {
            Handle handle;
            CreateEffectHook::Callback(user, Names[effect], &handle);
            Logging.Log("[botw-coop] Prodige : OUTIL DE TEST %s, cree=%d", Names[effect], handle.m_Effect != 0);
        }
    }
#endif

}
