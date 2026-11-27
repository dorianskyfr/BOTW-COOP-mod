#pragma once

#include <atomic>
#include <cstdint>

namespace coop::champion_fx {

    enum Effect : unsigned { Wind = 1, Lightning, Count };
    constexpr std::uint64_t MaxAgeMs = 800;
    constexpr unsigned ShieldCode = 3; /* KAGO 3..6 : bouclier de Daruk, 0..3 charges affichées */
    constexpr unsigned MaxShieldCharges = 3;
    constexpr std::uint64_t ShieldRefreshMs = 200;
    inline std::atomic<std::uint64_t> g_Local[Count] {};
    inline std::atomic<std::uint64_t> g_Remote[Count] {};
    inline std::atomic<unsigned> g_LocalShield { 0 };

    void Install();
    void SetLocalActor(std::uintptr_t actor);
    void PlayPending(std::uintptr_t puppet);
    void ReceiveShield(unsigned charges);
    void ForgetPuppet();
    void ClearRemote();

#ifdef COOP_TEST_CHAMPION_FX
    void DebugEmit(Effect effect);
#endif

}
