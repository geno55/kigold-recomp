// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Quest64-Recomp (https://github.com/Rainchus/Quest64-Recomp, GPL-3.0).
#ifndef KIG_RENDER_H
#define KIG_RENDER_H

#include <cstdint>
#include <memory>
#include <vector>

#include "ultramodern/renderer_context.hpp"

namespace RT64 {
    struct Application;
}

namespace kig::renderer {
    // ultramodern renderer backed by RT64 (adapted from Quest64-Recomp's rt64_render_context.cpp,
    // without the RmlUi render hooks and texture-pack support).
    class RT64Context final : public ultramodern::renderer::RendererContext {
    public:
        RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
        ~RT64Context() override;

        bool valid() override { return static_cast<bool>(app); }
        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                           const ultramodern::renderer::GraphicsConfig& new_config) override;
        void enable_instant_present() override;
        void send_dl(const OSTask* task) override;
        // A raw RDP command list from osDpSetNextBuffer (KI Gold builds some on the CPU).
        void send_rdp_list(uint32_t start, uint32_t end) override;
        void send_dummy_workload(uint32_t fb_address) override;
        void update_screen() override;
        void shutdown() override;
        uint32_t get_display_framerate() const override;
        float get_resolution_scale() const override;

    private:
        // RDP lists held until the next gfx task (or present), see send_rdp_list.
        struct PendingRdpList {
            uint32_t start = 0;
            std::vector<uint8_t> words;
        };
        void flush_rdp_lists(bool drop_final_sync);
        void wait_for_rdram_writeback();

        std::unique_ptr<RT64::Application> app;
        std::vector<PendingRdpList> pending_rdp_lists;
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}

#endif
