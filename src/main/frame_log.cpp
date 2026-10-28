// Frame log for the frame-pacing analysis.
//
// KIG_FRAME_LOG=<file> writes one line per event, both from the graphics thread (so in the order
// RT64 processes them):
//   D <t> <dl addr> <color image>   a gfx task, and the framebuffer it draws into
//   S <t> <VI origin>               a screen update (one per VI): what is being scanned out
// t is kig::input::script_time() (the input/capture clock). tools/frame_pacing.py turns the log
// into per-scene hold-length distributions, stale/flip-back counts and presented frame rates.
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "kig_frame_log.h"
#include "kig_input.h"
#include "kig_stats.h"

namespace {
    FILE* log_file = nullptr;
    std::once_flag init_flag;

    FILE* get_log() {
        std::call_once(init_flag, [] {
            if (const char* path = std::getenv("KIG_FRAME_LOG")) {
                log_file = std::fopen(path, "w");
            }
        });
        return log_file;
    }
}

void kig::framelog::on_display_list(const uint8_t* rdram, uint32_t dl_addr) {
    if (FILE* f = get_log()) {
        std::fprintf(f, "D %.4f %06X %06X\n", kig::input::script_time(), dl_addr,
                     kig::stats::main_color_image(rdram, dl_addr));
    }
}

void kig::framelog::on_rdp_list(const uint8_t* rdram, uint32_t start, uint32_t end) {
    if (FILE* f = get_log()) {
        uint32_t cimg = 0;
        for (uint32_t a = start & 0x7FFFF8; a + 8 <= (end & 0x7FFFFF) && a < (start & 0x7FFFFF) + 0x40; a += 8) {
            uint32_t w0 = *reinterpret_cast<const uint32_t*>(rdram + a);
            if ((w0 >> 24) == 0xFF) {
                cimg = *reinterpret_cast<const uint32_t*>(rdram + a + 4) & 0xFFFFFF;
                break;
            }
        }
        std::fprintf(f, "R %.4f %06X %06X\n", kig::input::script_time(), start & 0xFFFFFF, cimg);
    }
}

void kig::framelog::on_screen_update(uint32_t vi_origin) {
    if (FILE* f = get_log()) {
        std::fprintf(f, "S %.4f %06X\n", kig::input::script_time(), vi_origin & 0xFFFFFF);
    }
}
