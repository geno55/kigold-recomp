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

// src/game/kig_hooks.cpp: the display list pointer check for #7 (KIG_PTR_CHECK). _track runs before each
// store of the fight loop's display list head (0x8004A318), with the value stored and the store's
// address; _check runs on entry to the functions given the head's address. The first bad value is
// reported with the recent stores, the call chain and an RDRAM dump.
void kig_gfx_track(uint8_t* rdram, recomp_context* ctx, uint32_t site, gpr value);
void kig_gfx_check(uint8_t* rdram, uint32_t site);
// The same for the controller data pointers func_80011824 reads (index: the table index about to be used).
void kig_pad_check(uint8_t* rdram, recomp_context* ctx, uint32_t site, gpr index);

// src/game/kig_hooks.cpp: the rain (func_80028140), on entry and before its return. _end tags the
// raindrops for RT64 so the Expand aspect ratio draws them across the whole width.
void kig_rain_begin(uint8_t* rdram, recomp_context* ctx);
void kig_rain_end(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: the torch flames (func_800288BC). With the Expand aspect ratio the cull bounds
// widen, particles left of the screen move right, and each flame rectangle is tagged for RT64.
void kig_flame_cull_left(uint8_t* rdram, recomp_context* ctx);
void kig_flame_cull_right(uint8_t* rdram, recomp_context* ctx);
void kig_flame_rect_begin(uint8_t* rdram, recomp_context* ctx);
void kig_flame_rect_end(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: the fighter sprites (func_8001ECB4, also entered at 0x8001ED30). With the
// Expand aspect ratio the sprite moves a screen width right, the strip cull widens, and the strips are
// tagged for RT64 on the way out. _cull's reg is the bound's register (24 or 15).
void kig_fighter_begin(uint8_t* rdram, recomp_context* ctx);
void kig_fighter_move(uint8_t* rdram, recomp_context* ctx);
void kig_fighter_cull(uint8_t* rdram, recomp_context* ctx, int reg);
void kig_fighter_end(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: the large select and versus screen poses (columns: seg1_801BDB40, extra
// pieces: seg1_801BD180). With the Expand aspect ratio each rectangle keeps its x past the 4:3 edges and
// is tagged for RT64.
void kig_pose_column(uint8_t* rdram, recomp_context* ctx);
void kig_pose_rect(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: fight frame early exits. _frame (at seg2 0x801AE460's entry) runs the frame
// under setjmp and returns 1 when it has run (the hook then returns); _exit (before 0x801AE6B4's jr)
// leaves the frame from wherever it was jumped to.
int kig_fight_frame(uint8_t* rdram, recomp_context* ctx);
void kig_fight_frame_exit(uint8_t* rdram, recomp_context* ctx);

// src/game/cheats.cpp. _fight_frame: at the start of each fight frame (from kig_fight_frame), infinite
// health. The options tier: _loaded after seg1_801CD7F0 copies it from the loaded save
// (0x801CD82C), _reached before seg1_801ACB20 raises it to $v0 + 1 (0x801AD064), _save after
// seg1_801CDB4C reads it into $t1 for the save buffer (0x801CDC18).
void kig_cheats_fight_frame(uint8_t* rdram);
void kig_cheats_options_loaded(uint8_t* rdram, recomp_context* ctx);
void kig_cheats_options_reached(uint8_t* rdram, recomp_context* ctx);
void kig_cheats_options_save(uint8_t* rdram, recomp_context* ctx);

// src/game/kig_hooks.cpp: `mfc0 rt, Count` in game code (N64Recomp patch in patches/n64recomp/).
uint32_t recomp_cop0_count(void);

#ifdef __cplusplus
}
#endif

#endif
