// N64Recomp's recomp.h with TLB-aware memory access, for Killer Instinct Gold.
//
// KI Gold maps each fighter's data with the TLB (osMapTLB: virtual (id << 16) + 0x10000 onwards ->
// physical 0x1DB000 / 0x1E5000) and its code dereferences those kuseg addresses directly. The generated
// code addresses RDRAM as rdram + (vaddr - 0xFFFFFFFF80000000), which only works for KSEG0/KSEG1, so this
// header (found before the runtime's copy: include/ comes first on the include path) redefines the memory
// macros to send kuseg addresses through the page table that src/game/tlb.cpp keeps from osMapTLB.
// The cost is one well-predicted compare per access.
#ifndef KIG_RECOMP_H
#define KIG_RECOMP_H

#include "../lib/N64ModernRuntime/N64Recomp/include/recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// 4 KB pages over virtual 0x00000000-0x00FFFFFF; each entry is the physical page address or -1.
#define KIG_TLB_SPAN 0x01000000u
#define KIG_TLB_PAGES (KIG_TLB_SPAN >> 12)
extern int32_t kig_tlb_page[KIG_TLB_PAGES];

#ifdef __cplusplus
}
#endif

static inline uint64_t kig_addr(gpr a) {
    uint64_t off = a - 0xFFFFFFFF80000000ULL;
    if (__builtin_expect(off < 0x80000000ULL, 1)) {
        return off;  // KSEG0/KSEG1 (and above): as the stock macros
    }
    uint32_t v = (uint32_t)a;  // kuseg
    if (v < KIG_TLB_SPAN) {
        int32_t p = kig_tlb_page[v >> 12];
        if (p >= 0) {
            return (uint64_t)(uint32_t)p | (v & 0xFFF);
        }
    }
    return off;  // unmapped: outside committed memory, faults like the stock macros
}

#undef MEM_W
#undef MEM_H
#undef MEM_B
#undef MEM_HU
#undef MEM_BU
#undef SD
#undef LD

#define MEM_W(offset, reg) \
    (*(int32_t*)(rdram + kig_addr(((reg) + (offset)))))
#define MEM_H(offset, reg) \
    (*(int16_t*)(rdram + kig_addr(((reg) + (offset)) ^ 2)))
#define MEM_B(offset, reg) \
    (*(int8_t*)(rdram + kig_addr(((reg) + (offset)) ^ 3)))
#define MEM_HU(offset, reg) \
    (*(uint16_t*)(rdram + kig_addr(((reg) + (offset)) ^ 2)))
#define MEM_BU(offset, reg) \
    (*(uint8_t*)(rdram + kig_addr(((reg) + (offset)) ^ 3)))

#define SD(val, offset, reg) { \
    *(uint32_t*)(rdram + kig_addr(((reg) + (offset) + 4))) = (uint32_t)((gpr)(val) >> 0); \
    *(uint32_t*)(rdram + kig_addr(((reg) + (offset) + 0))) = (uint32_t)((gpr)(val) >> 32); \
}

// The stock helpers below were compiled against the stock macros; these copies use the ones above.
static inline uint64_t kig_load_doubleword(uint8_t* rdram, gpr reg, gpr offset) {
    uint64_t lo = (uint64_t)(uint32_t)MEM_W(reg, offset + 4);
    uint64_t hi = (uint64_t)(uint32_t)MEM_W(reg, offset + 0);
    return (lo << 0) | (hi << 32);
}

#define LD(offset, reg) \
    kig_load_doubleword(rdram, offset, reg)

static inline gpr kig_do_lwl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr word_address = address & ~0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return (gpr)(int32_t)(masked_value | loaded_value);
}

static inline gpr kig_do_lwr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr word_address = address & ~0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu >> (24 - misalignment * 8));
    loaded_value >>= (24 - misalignment * 8);
    return (gpr)(int32_t)(masked_value | loaded_value);
}

static inline void kig_do_swl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr word_address = address & ~0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu >> (misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) >> (misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline void kig_do_swr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr word_address = address & ~0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu << (24 - misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) << (24 - misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline gpr kig_do_ldl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~0x7;
    uint64_t loaded_value = kig_load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    gpr masked_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return masked_value | loaded_value;
}

static inline gpr kig_do_ldr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~0x7;
    uint64_t loaded_value = kig_load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    gpr masked_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu >> (56 - misalignment * 8));
    loaded_value >>= (56 - misalignment * 8);
    return masked_value | loaded_value;
}

static inline void kig_do_sdl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~0x7;
    uint64_t initial_value = kig_load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu >> (misalignment * 8));
    uint64_t shifted_input_value = val >> (misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (uint32_t)ret;
    MEM_W(0, dword_address + 0) = (uint32_t)(ret >> 32);
}

static inline void kig_do_sdr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~0x7;
    uint64_t initial_value = kig_load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu << (56 - misalignment * 8));
    uint64_t shifted_input_value = val << (56 - misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (uint32_t)ret;
    MEM_W(0, dword_address + 0) = (uint32_t)(ret >> 32);
}

#define do_lwl kig_do_lwl
#define do_lwr kig_do_lwr
#define do_swl kig_do_swl
#define do_swr kig_do_swr
#define do_ldl kig_do_ldl
#define do_ldr kig_do_ldr
#define do_sdl kig_do_sdl
#define do_sdr kig_do_sdr

// ---------------------------------------------------------------------------------------------
// Resumable calls, for save states (src/game/savestate.cpp; N64Recomp patch 0002 emits these).
// Each game thread keeps a shadow stack of the call sites it is in: the vram of each call instruction
// and, for an indirect call, the callee's vram. Restoring a thread rebuilds its native call chain from
// that list: each function's resume dispatch jumps back into the call it was in.
// ---------------------------------------------------------------------------------------------
#define RECOMP_SS_MAX 256

typedef struct {
    uint32_t site;
    uint32_t target;
} RecompSSEntry;

typedef struct {
    uint32_t depth;           // frames on the stack (entries beyond RECOMP_SS_MAX aren't kept)
    uint32_t resume_pos;      // while resuming: the next entry to re-enter
    uint32_t resume_len;      // nonzero while this thread is being resumed
    uint32_t target_pending;  // the next indirect call takes `target` instead of its register
    uint32_t target;
    RecompSSEntry e[RECOMP_SS_MAX];
} RecompShadowStack;

#if defined(_MSC_VER)
#define RECOMP_TLS __declspec(thread)
#else
#define RECOMP_TLS _Thread_local
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern volatile int recomp_resume_active;        // some thread is being resumed
RecompShadowStack* recomp_ss_self(void);         // this thread's (src/game/savestate_tls.c)
uint32_t recomp_resume_enter(uint8_t* rdram, recomp_context* ctx, uint32_t func_vram);
void recomp_resume_fail(uint8_t* rdram, recomp_context* ctx, uint32_t func_vram);
#ifdef __cplusplus
}
#endif

// The rest is for the recompiled C code, which reads the TLS variable directly (C++ code uses
// recomp_ss_self()).
#ifndef __cplusplus
extern RECOMP_TLS RecompShadowStack recomp_ss;

#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RECOMP_UNLIKELY(x) (x)
#endif

#define RECOMP_SS_PUSH(site_, target_) do { \
    RecompShadowStack* ss_ = &recomp_ss; \
    uint32_t d_ = ss_->depth++; \
    if (d_ < RECOMP_SS_MAX) { ss_->e[d_].site = (site_); ss_->e[d_].target = (uint32_t)(target_); } \
} while (0)

#define RECOMP_SS_POP() (recomp_ss.depth--)

#define RECOMP_RESUMING() (RECOMP_UNLIKELY(recomp_resume_active) && recomp_ss.resume_len != 0)

static inline gpr recomp_resume_take_target(void) {
    recomp_ss.target_pending = 0;
    return (gpr)(int32_t)recomp_ss.target;
}
// The callee of an indirect call: x, unless the call is being resumed (then x isn't evaluated: it may
// read a variable the resume jump skipped).
#define RECOMP_RESUME_TARGET(x) \
    ((RECOMP_UNLIKELY(recomp_resume_active) && recomp_ss.target_pending) ? recomp_resume_take_target() : (gpr)(x))
#endif  // !__cplusplus

#endif
