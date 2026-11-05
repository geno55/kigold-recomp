// Hooks called from recompiled code ([[patches.hook]] entries in tools/gen_config.py), plus the
// test-only helpers (each does nothing unless its environment variable is set).
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "kig_crash.h"
#include "kig_input.h"
#include "kig_log.h"
#include "kig_patches.h"
#include "kig_segments.h"
#include "kig_savestate.h"

// ---------------------------------------------------------------------------------------------
// Code segments. KI Gold's code is deflate-compressed in the ROM (docs/rom_map.md). The game DMAs a
// packed blob to RAM and inflates it with boot's func_80000900(src, dst, scratch), which returns the
// number of bytes written. librecomp only learns about loaded code through load_overlays, so the hooks
// below recognise a code segment by its packed bytes and register it at its destination; anything the
// inflater writes over a registered overlay unloads that overlay first (seg1 and seg2 share RAM).
// ---------------------------------------------------------------------------------------------
namespace {
    struct InflateCall {
        gpr src;  // sign-extended, as in the registers
        gpr dst;
    };
    thread_local InflateCall inflate_call{};

    struct LoadedOverlay {
        int32_t ram;
        uint32_t size;
        const char* name;
    };
    std::mutex overlay_mutex;
    std::vector<LoadedOverlay> loaded_overlays;

    // Which code segment (if any) the packed blob at `src` is: compares its first bytes with the ROM.
    const kig::CodeSegment* identify(uint8_t* rdram, gpr src) {
        std::span<const uint8_t> rom = recomp::get_rom();
        for (const kig::CodeSegment& seg : kig::code_segments) {
            if (seg.orig_rom + 64 > rom.size()) {
                continue;
            }
            bool match = true;
            for (int32_t i = 0; i < 64 && match; i++) {
                match = MEM_BU(i, src) == rom[seg.orig_rom + i];
            }
            if (match) {
                return &seg;
            }
        }
        return nullptr;
    }
}

extern "C" void kig_inflate_begin(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    inflate_call = { ctx->r4, ctx->r5 };
}

// Runs after the inflate call returned; v0 is not set yet (the function loads it next), so the
// byte count comes from the inflater's output counter (0x8000BF7C).
extern "C" void kig_inflate_end(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    const uint32_t out_size = (uint32_t)MEM_W(0, (gpr)(int32_t)kig::INFLATE_OUTCNT);
    const int32_t dst = (int32_t)inflate_call.dst;
    const kig::CodeSegment* seg = identify(rdram, inflate_call.src);

    std::lock_guard lock{ overlay_mutex };
    // Whatever was written over a registered overlay replaced it.
    for (auto it = loaded_overlays.begin(); it != loaded_overlays.end();) {
        if (dst < it->ram + (int32_t)it->size && dst + (int32_t)out_size > it->ram) {
            unload_overlays(it->ram, it->size);
            fprintf(stderr, "[kig] unloaded %s at %08X\n", it->name, (uint32_t)it->ram);
            it = loaded_overlays.erase(it);
        } else {
            ++it;
        }
    }
    static const bool log_inflate = std::getenv("KIG_LOG_INFLATE") != nullptr;
    if (log_inflate) {
        fprintf(stderr, "[kig] inflate t=%.2f %08X -> %08X (%X bytes)\n", kig::input::script_time(),
                (uint32_t)inflate_call.src, (uint32_t)dst, out_size);
    }
    if (seg == nullptr) {
        return;  // data
    }
    if (out_size != seg->size || (uint32_t)dst != seg->vram) {
        fprintf(stderr, "[kig] %s inflated to %08X (%X bytes), expected %08X (%X bytes)\n", seg->name, (uint32_t)dst, out_size,
                seg->vram, seg->size);
    }
    load_overlays(seg->recomp_rom, dst, seg->size);
    if (!seg->is_static) {
        loaded_overlays.push_back({ dst, seg->size, seg->name });
    }
    fprintf(stderr, "[kig] loaded %s at %08X\n", seg->name, (uint32_t)dst);
}

// s32 osDpSetNextBuffer(void* buf, u64 size), replacing libultra's (boot 0x80003BB0, renamed
// kig_osDpSetNextBuffer; its body is skipped by the hook at its entry). KI Gold's scheduler (boot
// func_80002420) starts a CPU-built RDP command list with it before each frame's gfx task; the runtime
// has no RDP, so the list goes to RT64 on the gfx thread (runtime patch 0006), which then raises the
// DP interrupt as the RDP would. Never busy: returns 0.
extern "C" void kig_dp_set_next_buffer(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t buf = (uint32_t)ctx->r4;
    const uint32_t size = (uint32_t)ctx->r7;  // low word of the u64 in a2:a3
    const uint32_t start = buf & 0x1FFFFFFF;
    static int logged = 0;
    static const bool dump = std::getenv("KIG_RDP_DUMP") != nullptr;  // every 60th list's words
    static uint32_t count = 0;
    static const uint32_t dump_from = std::getenv("KIG_RDP_DUMP_FROM") ? (uint32_t)std::atoi(std::getenv("KIG_RDP_DUMP_FROM")) : 0;
    if (logged < 8 || (dump && (dump_from ? (count >= dump_from && count < dump_from + 12) : count % 60 == 0))) {
        logged++;
        fprintf(stderr, "[kig] osDpSetNextBuffer(%08X, %X)%s", buf, size, dump ? ":" : "\n");
        if (dump) {
            for (uint32_t i = 0; i < size && i < 0x1000; i += 4) {
                fprintf(stderr, " %08X", (uint32_t)MEM_W(i, (gpr)(int32_t)buf));
            }
            fprintf(stderr, "\n");
        }
    }
    count++;
    ultramodern::submit_rdp_list(start, start + size);
    ctx->r2 = 0;
}

// s32 __osDpDeviceBusy(void): the RDP is never busy here. Referenced only from the replaced
// osDpSetNextBuffer's (unreachable) original body.
extern "C" void __osDpDeviceBusy_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = 0;
}

// ---------------------------------------------------------------------------------------------
// Fighter state restarts. The fight overlay's per-frame entity loop (seg2 0x801AFD44) jumps to each
// entity's state code (jr at 0x801AFD64), which ends by jumping back into the loop. Some routines that
// state code calls with jal, the animation-script interpreter (0x801BCA80) among them, can restart the
// fighter's state instead of returning: they jump to 0x801B20F4 (at 0x801BE74C and 0x801BE790),
// abandoning the jal'd frames. Recompiled, those frames would get control back once the rest of the
// frame had run and carry on with its final registers ($fp one past the object array), writing past
// it and sometimes crashing. So the dispatch runs the state code under setjmp, and the restart jumps
// longjmp back there and continue with 0x801B20F4, which is where the N64 ends up.
// ---------------------------------------------------------------------------------------------
extern "C" void seg2_801B20F4(uint8_t* rdram, recomp_context* ctx);

namespace {
    thread_local std::vector<jmp_buf*> restart_targets;  // innermost entity dispatch last
}

// KIG_DISPATCH_TRACE=<t0>,<t1> (debugging): logs each entity dispatch and its return between script
// times t0 and t1 (seconds).
static bool dispatch_trace_on() {
    static double t0 = -1, t1 = -1;
    static const bool parsed = [] {
        if (const char* e = std::getenv("KIG_DISPATCH_TRACE")) std::sscanf(e, "%lf,%lf", &t0, &t1);
        return true;
    }();
    (void)parsed;
    double t = kig::input::script_time();
    return t0 >= 0 && t >= t0 && t <= t1;
}

extern "C" void kig_dispatch_entity(uint8_t* rdram, recomp_context* ctx) {
    jmp_buf target;
    recomp_func_t* volatile fn = LOOKUP_FUNC(ctx->r6);
    const uint32_t fp = (uint32_t)ctx->r30;
    const bool trace = dispatch_trace_on();
    if (trace) {
        fprintf(stderr, "[kig] dispatch t=%.3f slot %d type %u -> %08X depth %zu gp %d\n", kig::input::script_time(),
                (int)((fp - 0x801D34C0u) / 0x100), MEM_BU(0, (gpr)(int32_t)fp), (uint32_t)ctx->r6, restart_targets.size(),
                (int)ctx->r28);
    }
    // Save states (src/game/savestate.cpp): this native frame can't be resumed, so it is marked on the
    // shadow stack (no state is taken while a thread is inside it), and a restart's longjmp skips the
    // pops of the frames it abandons, so the depth is reset after one.
    RecompShadowStack* ss = recomp_ss_self();
    const uint32_t ss_depth = ss->depth;
    if (ss_depth < RECOMP_SS_MAX) {
        ss->e[ss_depth] = { RECOMP_SS_NATIVE_FRAME, 0 };
    }
    ss->depth = ss_depth + 1;
    restart_targets.push_back(&target);
    if (setjmp(target) != 0) {
        fn = seg2_801B20F4;  // a restart jump; this dispatch is still the innermost
        ss->depth = ss_depth + 1;
    }
    fn(rdram, ctx);
    restart_targets.pop_back();
    ss->depth = ss_depth;
    if (trace) {
        fprintf(stderr, "[kig] dispatch return slot %d (fp now %08X gp %d)\n", (int)((fp - 0x801D34C0u) / 0x100),
                (uint32_t)ctx->r30, (int)ctx->r28);
    }
}

extern "C" void kig_restart_state(uint8_t* rdram, recomp_context* ctx) {
    if (restart_targets.empty()) {
        seg2_801B20F4(rdram, ctx);  // not under a dispatch: a plain jump (the hook returns after it)
        return;
    }
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 3) {
        fprintf(stderr, "[kig] fighter state restart at t=%.2f (fp=%08X)\n", kig::input::script_time(), (uint32_t)ctx->r30);
    }
    longjmp(*restart_targets.back(), 1);
}

// ---------------------------------------------------------------------------------------------
// Display list pointer check for #7 (KIG_PTR_CHECK; hooks in tools/gen_config.py). Crash reports show
// the fight loop's display list head (0x8004A318) holding 0x06000000, the first word of a G_DL command:
// something made the head point at itself, or wrote over it. The fight loop's two display list buffers
// start at the address in 0x8004A164, 0x73A0 bytes each; any head outside them is reported once, with
// the last stores, the call chain, every thread's stack and an RDRAM dump (logs/ptrcheck-rdram.bin,
// host-order words).
// ---------------------------------------------------------------------------------------------
namespace {
    constexpr uint32_t GfxHead = 0x8004A318, GfxBase = 0x8004A164, GfxBufferSize = 0x73A0;
    const bool gfx_check_on = std::getenv("KIG_PTR_CHECK") != nullptr;
    struct GfxStore {
        uint32_t site;
        uint32_t value;
    };
    constexpr int GfxRing = 32;
    GfxStore gfx_ring[GfxRing];
    std::atomic<uint32_t> gfx_ring_next{ 0 };
    std::atomic<bool> gfx_reported{ false };

    void gfx_validate(uint8_t* rdram, uint32_t site, uint32_t value, bool is_store) {
        const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)GfxBase);
        if (base < 0x80000000u || base >= 0x80800000u) {
            return;  // not set up yet
        }
        if ((value == 0 && !is_store) || (value >= base && value <= base + 2 * GfxBufferSize && (value & 7) == 0)) {
            return;  // (0 on entry: the fight loop hasn't run since boot)
        }
        if (gfx_reported.exchange(true)) {
            return;
        }
        std::fprintf(stderr, "[kig] display list check t=%.3f: head %08X %s %08X (buffers %08X-%08X)\n", kig::input::script_time(),
                     value, is_store ? "about to be stored at" : "found on entry to", site, base, base + 2 * GfxBufferSize);
        std::fprintf(stderr, "[kig]   words at 8004A300:");
        for (uint32_t a = 0x8004A300; a < 0x8004A340; a += 4) {
            std::fprintf(stderr, " %08X", (uint32_t)MEM_W(0, (gpr)(int32_t)a));
        }
        std::fprintf(stderr, "\n[kig]   last head stores (oldest first):");
        const uint32_t n = gfx_ring_next.load();
        for (uint32_t i = n > GfxRing ? n - GfxRing : 0; i < n; i++) {
            const GfxStore& s = gfx_ring[i % GfxRing];
            std::fprintf(stderr, "%s%08X<-%08X", (i % 6) == 0 ? "\n[kig]    " : " ", s.site, s.value);
        }
        RecompShadowStack* ss = recomp_ss_self();
        std::fprintf(stderr, "\n[kig]   call chain (call sites, outermost first):");
        for (uint32_t i = 0; i < ss->depth && i < RECOMP_SS_MAX; i++) {
            std::fprintf(stderr, " %08X", ss->e[i].site);
        }
        std::fprintf(stderr, "\n");
        const std::filesystem::path dump = kig::log::dir() / "ptrcheck-rdram.bin";
        if (FILE* f = std::fopen(dump.string().c_str(), "wb")) {
            std::fwrite(rdram, 1, 0x800000, f);
            std::fclose(f);
            std::fprintf(stderr, "[kig]   RDRAM saved to %s\n", dump.string().c_str());
        }
        kig::crash::dump_all_threads(stderr);
        kig::log::flush();
    }
}

extern "C" void kig_gfx_track(uint8_t* rdram, recomp_context* ctx, uint32_t site, gpr value) {
    if (!gfx_check_on || (int32_t)ctx->r22 != (int32_t)GfxHead) {
        return;
    }
    const uint32_t i = gfx_ring_next.fetch_add(1);
    gfx_ring[i % GfxRing] = { site, (uint32_t)value };
    gfx_validate(rdram, site, (uint32_t)value, true);
}

extern "C" void kig_gfx_check(uint8_t* rdram, uint32_t site) {
    if (!gfx_check_on) {
        return;
    }
    gfx_validate(rdram, site, (uint32_t)MEM_W(0, (gpr)(int32_t)GfxHead), false);
}

// The game's handwritten code reads COP0 Count directly (seg2); same clock as osGetCount. Its fight
// RNG (seg2 func_801B6578, state at 0x801D88A8) mixes Count in on every call, so fights depend on
// host timing. KIG_COUNT_SEED=<n> (testing) makes those reads a fixed sequence instead, so a seed
// replays the same fights.
extern "C" u32 osGetCount();
namespace {
    const char* count_seed_env = std::getenv("KIG_COUNT_SEED");
    std::atomic<uint32_t> count_state{ count_seed_env ? (uint32_t)std::strtoul(count_seed_env, nullptr, 0) * 2654435761u + 1 : 0u };
}
extern "C" uint32_t recomp_cop0_count(void) {
    if (count_seed_env == nullptr) {
        return osGetCount();
    }
    uint32_t x = count_state.load(std::memory_order_relaxed);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    count_state.store(x, std::memory_order_relaxed);
    return x;
}

// Save states (src/game/savestate.cpp): the seeded Count sequence and the loaded code segments.
uint32_t kig::savestate_hooks::get_count_state() {
    return count_state.load();
}

void kig::savestate_hooks::set_count_state(uint32_t state) {
    if (count_seed_env != nullptr) {
        count_state.store(state);
    }
}

std::vector<kig::savestate_hooks::Overlay> kig::savestate_hooks::get_overlays() {
    std::lock_guard lock{ overlay_mutex };
    std::vector<Overlay> out;
    for (const LoadedOverlay& o : loaded_overlays) {
        out.push_back({ o.ram, o.size, o.name });
    }
    return out;
}

void kig::savestate_hooks::set_overlays(const std::vector<Overlay>& overlays) {
    std::lock_guard lock{ overlay_mutex };
    loaded_overlays.clear();
    for (const Overlay& o : overlays) {
        loaded_overlays.push_back({ o.ram, o.size, o.name });
    }
}

// ---------------------------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------------------------

// Sticky MXCSR exception flags seen on threads passing through kig_spin_yield (bits 0-5: invalid,
// denormal operand, divide by zero, overflow, underflow, precision). Printed in the status line.
std::atomic<uint32_t> kig_fpu_flags{ 0 };

// KIG_POKE="<t0>-<t1>:<hex addr>:<b|h|w>=<value>,...": holds the value at that address from game time t0
// to t1 (seconds), to set up a test state.
static void poke(uint8_t* rdram) {
    struct Item { double t0, t1; uint32_t addr; char type; int32_t value; };
    static const std::vector<Item> items = [] {
        std::vector<Item> out;
        if (const char* spec = std::getenv("KIG_POKE")) {
            std::stringstream ss{ spec };
            std::string part;
            while (std::getline(ss, part, ',')) {
                Item it{};
                char type = 'w';
                if (std::sscanf(part.c_str(), "%lf-%lf:%x:%c=%d", &it.t0, &it.t1, &it.addr, &type, &it.value) == 5) {
                    it.type = type;
                    out.push_back(it);
                } else {
                    std::fprintf(stderr, "[kig] KIG_POKE: can't parse '%s'\n", part.c_str());
                }
            }
        }
        return out;
    }();
    const double t = kig::input::script_time();
    for (const Item& it : items) {
        if (t < it.t0 || t >= it.t1) continue;
        const gpr base = (int32_t)it.addr;
        switch (it.type) {
            case 'b': MEM_B(0, base) = (int8_t)it.value; break;
            case 'h': MEM_H(0, base) = (int16_t)it.value; break;
            default: MEM_W(0, base) = it.value; break;
        }
    }
}

// KIG_WATCH="<hex addr>:<b|h|w|f>,..." (byte, halfword, word, float): logs the values, with the game
// time, whenever one of them changes (checked on every display list, i.e. once per game frame).
// Also applies KIG_POKE.
void kig_watch(uint8_t* rdram) {
    poke(rdram);
    struct Item { uint32_t addr; char type; };
    static const std::vector<Item> items = [] {
        std::vector<Item> out;
        if (const char* spec = std::getenv("KIG_WATCH")) {
            std::stringstream ss{ spec };
            std::string part;
            while (std::getline(ss, part, ',')) {
                size_t colon = part.find(':');
                out.push_back({ (uint32_t)std::strtoul(part.substr(0, colon).c_str(), nullptr, 16),
                                colon == std::string::npos ? 'w' : part[colon + 1] });
            }
        }
        return out;
    }();
    if (items.empty()) return;
    static std::mutex mutex;
    static std::string last;
    std::string line;
    char buf[48];
    for (const Item& it : items) {
        const gpr base = (int32_t)it.addr;
        switch (it.type) {
            case 'b': std::snprintf(buf, sizeof(buf), " %08X=%d", it.addr, (int)MEM_B(0, base)); break;
            case 'h': std::snprintf(buf, sizeof(buf), " %08X=%d", it.addr, (int)MEM_H(0, base)); break;
            case 'f': {
                int32_t bits = MEM_W(0, base);
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                std::snprintf(buf, sizeof(buf), " %08X=%g", it.addr, f);
                break;
            }
            default: std::snprintf(buf, sizeof(buf), " %08X=%08X", it.addr, (uint32_t)MEM_W(0, base)); break;
        }
        line += buf;
    }
    std::lock_guard lock{ mutex };
    if (line != last) {
        last = line;
        std::fprintf(stderr, "[kig] watch t=%.2f%s\n", kig::input::script_time(), line.c_str());
    }
}

// librecomp/ultramodern: what yield_self_1ms does, in two steps, and the quit flag.
namespace ultramodern {
    void wait_for_external_message_timed(uint8_t* rdram, uint32_t millis);
    void check_running_queue(uint8_t* rdram);
}
extern std::atomic_bool exited;

extern "C" void kig_spin_yield(uint8_t* rdram) {
    kig_fpu_flags.fetch_or(_mm_getcsr() & 0x3F, std::memory_order_relaxed);
    ultramodern::wait_for_external_message_timed(rdram, 1);
    // On quit, librecomp frees RDRAM once the main game thread and the event threads are joined; a game
    // thread still spinning here would then read the freed running queue. Park it instead: process exit
    // ends it.
    if (exited.load()) {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    ultramodern::check_running_queue(rdram);
}
