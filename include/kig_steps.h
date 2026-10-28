#ifndef KIG_STEPS_H
#define KIG_STEPS_H

#include <cstdint>

// Step scripts (src/game/steps.cpp): KIG_STEPS=<file> runs a scenario's `steps` (tests/scenarios/*.toml,
// language in tools/refemu.py), synced on game state instead of wall-clock times, so the same script
// drives kig.exe and the reference emulator.
namespace kig::steps {
    // Reads KIG_STEPS (main thread, before the game starts). No-op without it.
    void init();
    bool active();
    // RDRAM, once the runtime has it (from the renderer's creation).
    void set_rdram(uint8_t* rdram);
    // VI thread, once per VI after the game started: runs steps until one blocks.
    void on_vi();
    // Game threads (kig::input::get_input): buttons and stick values the script holds on `port`.
    void apply(int port, uint16_t& buttons, float& x, float& y);
    // Main thread: true once the script finished (or failed); exit_code() is then 0, or 3 on a failure.
    bool done();
    int exit_code();
}

#endif
