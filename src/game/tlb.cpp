// TLB emulation for the recompiled code (include/recomp.h translates kuseg addresses through
// kig_tlb_page). KI Gold maps each fighter's data with osMapTLB (seg0 func_8001CF5C, seg1): 4 KB
// pages at virtual (fighter id << 16) + 0x10000 onwards -> physical 0x1DB000 (player 1) and 0x1E5000
// (player 2), plus virtual page 0 -> 0x3A4000. The runtime has no TLB, so these replace the libultra
// functions that N64Recomp skips.
#include <array>
#include <cstdio>
#include <mutex>

#include "recomp.h"

extern "C" int32_t kig_tlb_page[KIG_TLB_PAGES];
int32_t kig_tlb_page[KIG_TLB_PAGES];

namespace {
    struct Entry {
        bool valid = false;
        uint32_t vaddr = 0;
        uint32_t page_size = 0;
        int32_t even = -1;
        int32_t odd = -1;
    };
    std::array<Entry, 32> entries;
    std::mutex tlb_mutex;

    const bool init_done = [] {
        for (int32_t& p : kig_tlb_page) p = -1;
        return true;
    }();

    void rebuild() {
        for (int32_t& p : kig_tlb_page) p = -1;
        for (const Entry& e : entries) {
            if (!e.valid) continue;
            for (int half = 0; half < 2; half++) {
                int32_t paddr = half ? e.odd : e.even;
                if (paddr == -1) continue;
                uint32_t v0 = e.vaddr + half * e.page_size;
                for (uint32_t off = 0; off < e.page_size; off += 0x1000) {
                    uint32_t v = v0 + off;
                    if (v < KIG_TLB_SPAN) {
                        kig_tlb_page[v >> 12] = (int32_t)(((uint32_t)paddr & 0x1FFFFFFF) + off);
                    }
                }
            }
        }
    }
}

// void osMapTLB(s32 index, OSPageMask pm, void* vaddr, u32 evenpaddr, u32 oddpaddr, s32 asid)
extern "C" void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t index = (uint32_t)ctx->r4 & 31;
    const uint32_t pm = (uint32_t)ctx->r5;
    Entry e;
    e.valid = true;
    e.page_size = ((pm | 0x1FFF) + 1) >> 1;
    e.vaddr = (uint32_t)ctx->r6 & ~(2 * e.page_size - 1);
    e.even = (int32_t)ctx->r7;
    e.odd = MEM_W(0x10, ctx->r29);
    std::lock_guard lock{ tlb_mutex };
    entries[index] = e;
    rebuild();
    static int logged = 0;
    if (logged < 64 || index == 1 || index == 6) {  // after the first few, each fighter's first page
        logged++;
        fprintf(stderr, "[kig] osMapTLB %u: %08X (+%X x2) -> even %08X odd %08X\n", index, e.vaddr, e.page_size,
                (uint32_t)e.even, (uint32_t)e.odd);
    }
}

// void osUnmapTLB(s32 index)
extern "C" void osUnmapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    std::lock_guard lock{ tlb_mutex };
    entries[(uint32_t)ctx->r4 & 31].valid = false;
    rebuild();
}

// u32 __osProbeTLB(void* vaddr): physical address, or -1 if unmapped.
extern "C" void __osProbeTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t v = (uint32_t)ctx->r4;
    int32_t p = v < KIG_TLB_SPAN ? kig_tlb_page[v >> 12] : -1;
    ctx->r2 = p < 0 ? (gpr)(int32_t)-1 : (gpr)(int32_t)((uint32_t)p | (v & 0xFFF));
}
