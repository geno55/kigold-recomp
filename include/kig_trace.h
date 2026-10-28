#ifndef KIG_TRACE_INTERNAL_H
#define KIG_TRACE_INTERNAL_H

#include <cstdio>

namespace kig::trace {
    // Reads KIG_TRACE / KIG_TRACE_FILE / KIG_COVERAGE_FILE. Call at the start of main().
    void init();
    // Prints the calling thread's recent trace events (only populated in --trace builds).
    void dump_current_thread(FILE* out);
}

#endif
