#include "notify.hpp"

#include "clock.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::notify {

    namespace {

        constexpr std::uintptr_t ShowInfo = 0xe7a780;   /* (int idx) */
        constexpr std::uintptr_t GetMessage = 0xe84870; /* int (const SafeString* file, const SafeString* label, MessageString* out) */
        constexpr int InfoIndex = 0x21;                 /* libelle « 0028 » (table main+0x1daa3b0) */
        constexpr std::uint64_t OverrideMs = 2000;
        constexpr std::uint64_t SpacingMs = 3500;       /* entre deux messages */
        constexpr int MaxChars = 46;

        struct SafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        };
        /* Sortie de getMessage (main+0xf6a1b8 copie +0x0 et +0x8) : texte UTF-16 et nombre de caracteres. */
        struct MessageString {
            const char16_t* m_Text;
            std::uint32_t m_Length;
        };

        /* File simple : 8 messages, un producteur protege par un verrou tournant. */
        constexpr int QueueSize = 8;
        char s_Queue[QueueSize][MaxChars * 3 + 1];
        std::atomic<int> s_Head { 0 }, s_Tail { 0 };
        std::atomic_flag s_Lock = ATOMIC_FLAG_INIT;

        char16_t s_Text[MaxChars + 1];
        std::uint32_t s_TextLen = 0;
        std::atomic<std::uint64_t> s_OverrideUntil { 0 };
        std::uint64_t s_NextShow = 0;
        bool s_Installed = false;

        std::uint32_t ToUtf16(const char* in, char16_t* out, int cap) {
            std::uint32_t n = 0;
            const auto* s = reinterpret_cast<const unsigned char*>(in);
            while (*s != 0 && n + 1 < static_cast<std::uint32_t>(cap)) {
                std::uint32_t c = *s++;
                if (c >= 0xe0 && s[0] != 0 && s[1] != 0) {
                    c = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
                    s += 2;
                } else if (c >= 0xc0 && s[0] != 0) {
                    c = ((c & 0x1f) << 6) | (s[0] & 0x3f);
                    s += 1;
                }
                out[n++] = static_cast<char16_t>(c);
            }
            out[n] = 0;
            return n;
        }

        /*
         * Les deux conditions de main+0xe7a790 (lu dans notre image de main) : sans elles, la
         * demande est ignoree et le message perdu. 1) main+0xe89ae0 : bit 4 de l'octet
         * [[main+0x1d67908]] + 0x64c31 doit etre a 0 ; 2) main+0xe7a240 (ecran principal) non nul.
         */
        bool BannerAvailable(std::uintptr_t base) {
            const auto got = *reinterpret_cast<const std::uintptr_t*>(base + 0x1d67908);
            const auto ui = got != 0 ? *reinterpret_cast<const std::uintptr_t*>(got) : 0;
            if (ui == 0 || ((*reinterpret_cast<const std::uint8_t*>(ui + 0x64c31) >> 4) & 1) != 0)
                return false;
            using MainScreenFn = std::uintptr_t (*)();
            return reinterpret_cast<MainScreenFn>(base + 0xe7a240)() != 0;
        }

        bool Is(const SafeString* s, const char* text) {
            return s != nullptr && s->m_Str != nullptr && std::strcmp(s->m_Str, text) == 0;
        }

        HOOK_DEFINE_TRAMPOLINE(GetMessageHook) {
            static int Callback(const SafeString* file, const SafeString* label, MessageString* out) {
                if (out != nullptr && coop::clock::NowMs() < s_OverrideUntil.load(std::memory_order_relaxed)
                    && Is(label, "0028") && Is(file, "LayoutMsg/MainScreen_00")) {
                    out->m_Text = s_Text;
                    out->m_Length = s_TextLen;
                    return 0;
                }
                return Orig(file, label, out);
            }
        };

    }

    void Install() {
        GetMessageHook::InstallAtOffset(GetMessage);
        s_Installed = true;
        Logging.Log("[botw-coop] messages : crochet pose sur main+0x%lx", GetMessage);
    }

    void Post(const char* text) {
        while (s_Lock.test_and_set(std::memory_order_acquire)) { }
        const int head = s_Head.load(std::memory_order_relaxed);
        const int next = (head + 1) % QueueSize;
        if (next != s_Tail.load(std::memory_order_acquire)) {
            std::strncpy(s_Queue[head], text, sizeof(s_Queue[head]) - 1);
            s_Queue[head][sizeof(s_Queue[head]) - 1] = '\0';
            s_Head.store(next, std::memory_order_release);
        }
        s_Lock.clear(std::memory_order_release);
        Logging.Log("[botw-coop] messages : \"%s\"", text);
    }

    void OnFrame() {
        static std::uint64_t s_ReadySince = 0;
        if (!s_Installed || !coop::game::IsWorldReady()) {
            s_ReadySince = 0;
            return;
        }
        const std::uint64_t now = coop::clock::NowMs();
        if (s_ReadySince == 0)
            s_ReadySince = now;
        /* Pas dans les 12 premieres secondes en jeu : le « monde pret » du plugin arrive avant la fin du
           fondu au noir qui suit le chargement, et un bandeau demande pendant le fondu ne se voit pas
           (essais nt2 a nt4, NOTES 5.29). Valeur choisie d'apres ces essais. */
        if (now < s_NextShow || now - s_ReadySince < 12000)
            return;
        const int tail = s_Tail.load(std::memory_order_relaxed);
        if (tail == s_Head.load(std::memory_order_acquire))
            return;
        /* Jeu en pause (aide du jeu, menu) : le bandeau demande alors n'apparait pas (essai bw2 : don
           recu pendant l'aide « Change Bow », rien vu ensuite). Pause reperee au minuteur de lune de
           sang, qui avance en jeu et s'arrete en pause (journal de bw2 : 205,6 fige pendant l'aide).
           Au plus 120 s d'attente, au cas ou il ne bougerait plus pour une autre raison. */
        static float s_Moon = -1.0f;
        static std::uint64_t s_MoonMoved = 0, s_WaitSince = 0, s_MoonRead = 0;
        if (coop::game::WorldState w; coop::game::ReadWorld(w)) {
            /* Relu seulement quand un message attend : une valeur ancienne ne prouve rien. */
            if (now - s_MoonRead > 200)
                s_Moon = -1.0f, s_MoonMoved = 0;
            s_MoonRead = now;
            if (s_Moon < 0.0f)
                s_Moon = w.m_BloodMoonTimer;
            else if (w.m_BloodMoonTimer != s_Moon) {
                s_Moon = w.m_BloodMoonTimer;
                s_MoonMoved = now;
            }
            if (s_WaitSince == 0)
                s_WaitSince = now;
            if (now - s_MoonMoved > 300 && now - s_WaitSince < 120000)
                return;
        }
        s_WaitSince = 0;
        const auto base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        if (!BannerAvailable(base)) {
            static std::uint64_t s_Busy = 0;
            if (s_Busy++ % 150 == 0)
                Logging.Log("[botw-coop] messages : bandeau indisponible pour l'instant, nouvel essai (n.%lu)", s_Busy);
            return;
        }
        s_TextLen = ToUtf16(s_Queue[tail], s_Text, MaxChars + 1);
        s_Tail.store((tail + 1) % QueueSize, std::memory_order_release);
        s_OverrideUntil.store(now + OverrideMs, std::memory_order_relaxed);
        s_NextShow = now + SpacingMs;
        using ShowFn = void (*)(int);
        reinterpret_cast<ShowFn>(base + ShowInfo)(InfoIndex);
        Logging.Log("[botw-coop] messages : bandeau demande (%u caracteres)", s_TextLen);
    }

}
