#pragma once

#include "lib.hpp"

#include <cstdint>

/*
 * Horloge monotone commune a tous les fils du plugin : compteur systeme ARMv8
 * (cntpct_el0 via svcGetSystemTick) et sa frequence (cntfrq_el0, 19,2 MHz
 * mesures sous Ryujinx).
 */
namespace coop::clock {

    inline std::uint64_t TickFreq() {
        static std::uint64_t freq = 0;
        if (freq == 0) {
            std::uint64_t value;
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
            freq = value != 0 ? value : 19200000;
        }
        return freq;
    }

    /* Microsecondes depuis le demarrage du systeme emule (mesures de duree). */
    inline std::uint64_t NowUs() {
        const std::uint64_t ticks = svcGetSystemTick();
        const std::uint64_t freq = TickFreq();
        return (ticks / freq) * 1000000 + (ticks % freq) * 1000000 / freq;
    }

    /* Millisecondes depuis le demarrage du systeme emule. */
    inline std::uint64_t NowMs() {
        return svcGetSystemTick() * 1000 / TickFreq();
    }

}
