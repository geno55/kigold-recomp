#ifndef KIG_FRAME_LOG_H
#define KIG_FRAME_LOG_H

#include <cstdint>

namespace kig::framelog {
    // KIG_FRAME_LOG (src/main/frame_log.cpp). Both are called on the graphics thread.
    void on_display_list(const uint8_t* rdram, uint32_t dl_addr);
    void on_screen_update(uint32_t vi_origin);
    // A raw RDP list (osDpSetNextBuffer): "R <t> <start> <color image it sets, or 0>".
    void on_rdp_list(const uint8_t* rdram, uint32_t start, uint32_t end);
}

#endif
