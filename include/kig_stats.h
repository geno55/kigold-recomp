#ifndef KIG_STATS_H
#define KIG_STATS_H

#include <atomic>
#include <cstdint>

namespace kig::stats {
    // Bring-up counters, printed periodically by main.cpp's update_gfx.
    extern std::atomic<uint64_t> display_lists;
    extern std::atomic<uint64_t> rdp_lists;             // raw RDP lists (osDpSetNextBuffer)
    extern std::atomic<uint64_t> screen_updates;
    extern std::atomic<uint64_t> audio_tasks;
    extern std::atomic<uint64_t> audio_buffers;
    extern std::atomic<uint64_t> origin_changes;        // VI origin differs from the previous screen update
    extern std::atomic<uint64_t> audio_peak;            // max |sample| since the last status line
    extern std::atomic<uint64_t> audio_samples;         // total int16 samples received by queue_samples
    extern std::atomic<uint64_t> audio_out_peak_milli;  // max |float| queued to SDL x1000 since the last status line
    extern std::atomic<uint64_t> audio_out_bytes;       // total bytes queued to SDL

    // Prints a summary of a gfx task's display list when KIG_DL_STATS is set (src/main/dl_stats.cpp).
    void summarize_dl(const uint8_t* rdram, uint32_t dl_addr);
    // Saves RDRAM at chosen gfx tasks when KIG_RDRAM_DUMP is set (src/main/dl_stats.cpp).
    void dump_rdram(const uint8_t* rdram, uint32_t dl_addr, uint32_t rdp_start, uint32_t rdp_end);
    // The color image a display list draws most into (src/main/dl_stats.cpp; frame log).
    uint32_t main_color_image(const uint8_t* rdram, uint32_t dl_addr);
}

#endif
