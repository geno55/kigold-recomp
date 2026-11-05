// The shadow stack of call sites (include/recomp.h), defined in C so the recompiled C code reads it
// directly as a plain TLS variable.
#include "recomp.h"

RECOMP_TLS RecompShadowStack recomp_ss;
volatile int recomp_resume_active = 0;

RecompShadowStack* recomp_ss_self(void) {
    return &recomp_ss;
}
