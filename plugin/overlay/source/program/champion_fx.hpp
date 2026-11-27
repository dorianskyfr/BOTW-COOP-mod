#pragma once

#include <atomic>
#include <cstdint>

namespace coop::champion_fx {

    enum Effect : unsigned { Wind = 1, Lightning, Count };
    constexpr std::uint64_t MaxAgeMs = 800;
    inline std::atomic<std::uint64_t> g_Local[Count] {};
    inline std::atomic<std::uint64_t> g_Remote[Count] {};

    void Install();
    void SetLocalActor(std::uintptr_t actor);
    void PlayPending(std::uintptr_t puppet);
    void ClearRemote();

#ifdef COOP_TEST_CHAMPION_FX
    void DebugEmit(Effect effect);
#endif

}
