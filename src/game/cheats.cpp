// Cheats, set in the Cheats tab (cheats.json, all off by default). Addresses are USA Rev 2.
//
// Fight (seg2's BSS, so only written during a fight frame: seg1 occupies that RAM elsewhere):
//   0x801D3514  player 1's health, 16 bits, 0x7800 = a full bar (player 2's is at 0x801D3614). The
//               byte at 0x801D354F says whether player 1 is on the second bar.
//
// Unlocks (seg0's data, always present). The attract mode's button codes (seg1 0x801C3AAC, table at
// 0x802854EC) set them:
//   0x80127C28  1 = Gargos playable (Z A R Z A B). Not saved.
//   0x8012823C  Options tiers unlocked, 6 = all (Z B A L A Z). Saved: seg1_801CDB4C copies it into the
//               save buffer (0x802D384A) and seg1_801CD7F0 copies it back on load; beating the game
//               raises it (seg1_801ACB20).
//
// While a cheat is on its byte is forced every frame. The game's own value is kept, so turning the cheat
// off puts it back. The options tier's is also what goes into the save, so the cheat never reaches it.
#include <cstdio>

#include "recomp.h"
#include "kig_cheats.h"
#include "kig_config.h"
#include "kig_patches.h"

namespace {
    uint8_t* rdram_base = nullptr;

    uint8_t read8(uint8_t* rdram, uint32_t addr) { return rdram[(addr & 0x7FFFFF) ^ 3]; }
    void write8(uint8_t* rdram, uint32_t addr, uint8_t v) { rdram[(addr & 0x7FFFFF) ^ 3] = v; }

    // A byte forced while its cheat is on. `real` is the game's own value: what the game last wrote,
    // or what it was before the cheat.
    struct Forced {
        uint32_t addr;
        uint8_t on_value;
        bool forcing = false;
        uint8_t real = 0;

        void update(uint8_t* rdram, bool on) {
            const uint8_t cur = read8(rdram, addr);
            if (!forcing || cur != on_value) {
                real = cur;  // the game's value (or the game wrote over the forced one)
            }
            if (on) {
                write8(rdram, addr, on_value);
                if (!forcing) std::fprintf(stderr, "[kig] cheats: %08X forced to %u (was %u)\n", addr, on_value, real);
            } else if (forcing) {
                write8(rdram, addr, real);
                std::fprintf(stderr, "[kig] cheats: %08X back to %u\n", addr, real);
            }
            forcing = on;
        }
    };

    constexpr uint32_t P1Health = 0x801D3514;
    Forced gargos{ 0x80127C28, 1 };
    Forced options{ 0x8012823C, 6 };
}

void kig::cheats::set_rdram(uint8_t* rdram) {
    rdram_base = rdram;
}

void kig::cheats::on_input() {
    if (rdram_base == nullptr) {
        return;
    }
    gargos.update(rdram_base, kig::config::cheat("unlock_gargos"));
    options.update(rdram_base, kig::config::cheat("unlock_options"));
}

extern "C" void kig_cheats_fight_frame(uint8_t* rdram) {
    if (kig::config::cheat("infinite_health_p1")) {
        write8(rdram, P1Health, 0x78);
        write8(rdram, P1Health + 1, 0x00);
    }
}

extern "C" void kig_cheats_options_loaded(uint8_t* rdram, recomp_context*) {
    options.real = read8(rdram, options.addr);
}

extern "C" void kig_cheats_options_reached(uint8_t*, recomp_context* ctx) {
    const int tier = (int)(int32_t)ctx->r2 + 1;
    if (tier > options.real && tier < 256) {
        options.real = (uint8_t)tier;
    }
}

extern "C" void kig_cheats_options_save(uint8_t*, recomp_context* ctx) {
    if (options.forcing) {
        std::fprintf(stderr, "[kig] cheats: saving the options tier as %u, not %u\n", options.real, (unsigned)ctx->r9);
        ctx->r9 = options.real;
    }
}
