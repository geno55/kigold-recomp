// Save states (src/game/savestate.cpp).
//
// A save state holds RDRAM and everything native that the game's state depends on: each game thread's
// registers and native call chain (the shadow stack of call sites, include/recomp.h), the scheduler's
// running queue, the event registrations and VI state, the TLB, the loaded code segments and the seeded
// Count sequence. The idle thread takes and loads states at the top of its loop, when every other game
// thread is blocked in a message queue and no RSP task is running; a request waits for such a moment.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "recomp.h"

namespace kig::savestate {
    // Hooks into ultramodern; call once before the game starts.
    void install();
    // librecomp's thread_create_callback: registers the new thread's registers.
    void on_thread_create(uint8_t* rdram, recomp_context* ctx);

    // Asynchronous: the state is saved or loaded at the next safe point. Results are logged and
    // returned by last_result(). A load reads the file at once and fails early if it can't.
    // A save can wait for a RAM value: the first safe point where the big-endian 8/16/32-bit value at
    // addr (KSEG0) equals value (addr 0: none). Saving at a given game frame makes runs comparable.
    struct SaveCondition {
        uint32_t addr = 0;
        int bits = 32;
        uint32_t value = 0;
    };
    // quiet: no toast for the result (automatic saves).
    void request_save(const std::filesystem::path& path, const SaveCondition& condition = {}, bool quiet = false);
    bool request_load(const std::filesystem::path& path, std::string* error = nullptr);
    bool busy();  // a request is waiting for a safe point

    struct Result {
        uint64_t serial = 0;  // increments with every finished request
        bool ok = false;
        bool was_load = false;
        bool quiet = false;
        std::string message;
    };
    Result last_result();

    // The quick-save slot in the app folder (F5 saves, F7 loads).
    void set_slot_dir(const std::filesystem::path& dir);
    std::filesystem::path slot_path();
}

// Native state elsewhere in kig that a save state includes.
namespace kig::savestate_hooks {
    uint32_t get_count_state();
    void set_count_state(uint32_t state);

    struct Overlay {
        int32_t ram;
        uint32_t size;
        const char* name;
    };
    std::vector<Overlay> get_overlays();
    void set_overlays(const std::vector<Overlay>& overlays);

    struct TlbEntry {
        uint32_t valid;
        uint32_t vaddr;
        uint32_t page_size;
        int32_t even;
        int32_t odd;
    };
    std::vector<TlbEntry> get_tlb();
    void set_tlb(const std::vector<TlbEntry>& entries);
}
