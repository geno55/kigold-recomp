#ifndef KIG_TRACE_H
#define KIG_TRACE_H

// Included by recompiled code when N64Recomp's trace_mode is on (tools/gen_config.py --trace, CMake
// -DKIG_TRACE=ON). N64Recomp emits `TRACE_ENTRY()` at the start of every function and `TRACE_RETURN()`
// before every `return;`, as bare statements, so both macros expand to complete statements.
//
// Coverage: each function reports its first entry once (a static flag per function, so the
// steady-state cost is one load and branch); KIG_COVERAGE_FILE collects the names.
// Call tracing: the per-thread ring / KIG_TRACE_FILE stream only run when kig_trace_on is set
// (KIG_TRACE=1 or KIG_TRACE_FILE; see src/main/trace.cpp).

#ifdef __cplusplus
extern "C" {
#endif

extern int kig_trace_on;
void kig_coverage_hit(const char* func);
void kig_trace_entry(const char* func);
void kig_trace_return(const char* func);

#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() {                                                     \
    static int kig_cov_seen_;                                              \
    if (!kig_cov_seen_) { kig_cov_seen_ = 1; kig_coverage_hit(__func__); } \
    if (kig_trace_on) kig_trace_entry(__func__);                          \
}
#define TRACE_RETURN() { if (kig_trace_on) kig_trace_return(__func__); }

#endif
