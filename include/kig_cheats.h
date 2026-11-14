#pragma once
// Cheats (the Cheats tab; options in cheats.json). src/game/cheats.cpp.
#include <cstdint>

namespace kig::cheats {
    // RDRAM, for on_input (set with the renderer, like kig::steps::set_rdram).
    void set_rdram(uint8_t* rdram);
    // Once per controller read, on the game thread: the unlocks.
    void on_input();
}

// The hooks the game calls (kig_cheats_*) are declared in kig_patches.h.
