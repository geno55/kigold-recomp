// Runtime functions called from N64Recomp [[patches.hook]] text (see tools/gen_config.py).
#ifndef KIG_PATCHES_H
#define KIG_PATCHES_H

#ifdef __cplusplus
extern "C" {
#endif

// src/game/kig_hooks.cpp: around boot's inflater func_80000900(src, dst, scratch). _begin (at its entry)
// records the arguments; _end (after the inflate call returns) registers the code segment the blob
// was, if any, with librecomp at its destination, and unloads overlays the output overwrote.
void kig_inflate_begin(uint8_t* rdram, recomp_context* ctx);
void kig_inflate_end(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: the busy-wait hook, for loops that on the N64 rely on preemption. Does what
// ultramodern's yield_self_1ms does: waits up to 1ms for an external (interrupt) message, delivers it,
// then switches to a higher-priority ready thread if there is one; once the game is quitting it parks
// the thread instead.
void kig_spin_yield(uint8_t* rdram);

// src/game/kig_hooks.cpp: replaces osDpSetNextBuffer (raw RDP lists go to RT64).
void kig_dp_set_next_buffer(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: fighter state restarts. _dispatch_entity replaces the entity loop's jump to
// an entity's state code (seg2 0x801AFD64); _restart_state replaces the jumps to 0x801B20F4 that leave
// jal'd routines, going back to that dispatch first.
void kig_dispatch_entity(uint8_t* rdram, recomp_context* ctx);
void kig_restart_state(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: `mfc0 rt, Count` in game code (N64Recomp patch in patches/n64recomp/).
uint32_t recomp_cop0_count(void);

#ifdef __cplusplus
}
#endif

#endif
