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
        constexpr std::uintptr_t FadeEffect = 0x56d010;
        constexpr std::uintptr_t PlayerShieldHandle = 0x2360;
        constexpr const char* Names[Count] = { nullptr, "Kago_Wind", "Kago_Lightning" };
        constexpr const char* ShieldNames[] = { nullptr, "Kago_ShieldCount_01", "Kago_ShieldCount_02", "Kago_ShieldCount_03" };
        std::atomic<std::uintptr_t> s_LocalUser { 0 };
        std::atomic<std::uintptr_t> s_LocalShieldHandle { 0 };
        std::atomic<std::uint64_t> s_RemoteShield { 0 };

        struct Handle {
            std::uintptr_t m_Effect = 0;
            std::uint32_t m_Id = 0;
        };

        std::uintptr_t s_ShieldUser = 0;
        Handle s_ShieldHandle;
        unsigned s_AppliedCharges = 0;

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
                for (unsigned charges = 1; charges <= MaxShieldCharges; charges++) {
                    if (std::strcmp(name, ShieldNames[charges]) == 0) {
                        g_LocalShield.store(charges, std::memory_order_relaxed);
                        return;
                    }
                }
            }
        };

        /* Le joueur relâche la garde : le jeu arrête l'effet dont la poignée est à actor+0x2360 (1.9.0). */
        HOOK_DEFINE_TRAMPOLINE(FadeEffectHook) {
            static Handle* Callback(Handle* handle) {
                if (reinterpret_cast<std::uintptr_t>(handle) == s_LocalShieldHandle.load(std::memory_order_relaxed))
                    g_LocalShield.store(0, std::memory_order_relaxed);
                return Orig(handle);
            }
        };

        void UpdateShield(std::uintptr_t user) {
            if (user != s_ShieldUser) {
                ForgetPuppet();
                s_ShieldUser = user;
            }
            const auto message = s_RemoteShield.load(std::memory_order_relaxed);
            const auto age = coop::clock::NowMs() - (message >> 2);
            const unsigned charges = age <= MaxAgeMs ? message & MaxShieldCharges : 0;
            if (charges == s_AppliedCharges)
                return;
            if (const auto effect = s_ShieldHandle.m_Effect; effect != 0 &&
                coop::game::CanReadMemory(effect + 0x20, sizeof(std::uint32_t)) &&
                *reinterpret_cast<const std::uint32_t*>(effect + 0x20) == s_ShieldHandle.m_Id) {
                FadeEffectHook::Orig(&s_ShieldHandle);
                Logging.Log("[botw-coop] Prodige : bouclier de Daruk retire pour le partenaire");
            }
            s_ShieldHandle = {};
            s_AppliedCharges = charges;
            if (charges != 0 && user != 0) {
                CreateEffectHook::Orig(user, ShieldNames[charges], &s_ShieldHandle);
                if (s_ShieldHandle.m_Effect != 0)
                    Logging.Log("[botw-coop] Prodige : bouclier de Daruk affiche pour le partenaire (%u)", charges);
            }
        }
    }

    void Install() {
        CreateEffectHook::InstallAtOffset(CreateEffect);
        FadeEffectHook::InstallAtOffset(FadeEffect);
    }

    void SetLocalActor(std::uintptr_t actor) {
        const auto user = EffectUser(actor);
        if (s_LocalUser.exchange(user, std::memory_order_relaxed) != user)
            g_LocalShield.store(0, std::memory_order_relaxed);
        s_LocalShieldHandle.store(user != 0 ? actor + PlayerShieldHandle : 0, std::memory_order_relaxed);
    }

    void PlayPending(std::uintptr_t puppet) {
        const auto user = EffectUser(puppet);
        UpdateShield(user);
        for (unsigned effect = Wind; effect < Count; effect++) {
            const auto received = g_Remote[effect].exchange(0, std::memory_order_relaxed);
            if (received == 0 || coop::clock::NowMs() - received > MaxAgeMs)
                continue;
            if (user != 0) {
                Handle handle;
                CreateEffectHook::Orig(user, Names[effect], &handle);
                if (handle.m_Effect != 0)
                    Logging.Log("[botw-coop] Prodige : %s affiche pour le partenaire", Names[effect]);
            }
        }
    }

    void ReceiveShield(unsigned charges) {
        s_RemoteShield.store((coop::clock::NowMs() << 2) | charges, std::memory_order_relaxed);
    }

    void ForgetPuppet() {
        s_ShieldUser = 0;
        s_ShieldHandle = {};
        s_AppliedCharges = 0;
    }

    void ClearRemote() {
        s_RemoteShield.store(0, std::memory_order_relaxed);
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
