// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Quest64-Recomp (https://github.com/Rainchus/Quest64-Recomp, GPL-3.0).
// RT64-backed ultramodern renderer. Adapted from Quest64-Recomp (src/main/rt64_render_context.cpp):
// same RT64 core/VI register wiring and config mapping, minus the texture-pack/mod plumbing, which this
// project doesn't use (yet). The launcher/menu draw through RT64's render hooks (src/main/ui_render.cpp).
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "hle/rt64_application.h"

#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "kig_crash.h"
#include "kig_capture.h"
#include "kig_frame_log.h"
#include "kig_render.h"
#include "kig_stats.h"
#include "kig_steps.h"
#include "kig_ui.h"

// src/game/test_hooks.cpp: KIG_WATCH / KIG_POKE, run once per display list.
void kig_watch(uint8_t* rdram);

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG = 0;
static unsigned int DPC_START_REG = 0;
static unsigned int DPC_END_REG = 0;
static unsigned int DPC_CURRENT_REG = 0;
static unsigned int DPC_STATUS_REG = 0;
static unsigned int DPC_CLOCK_REG = 0;
static unsigned int DPC_BUFBUSY_REG = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG = 0;

static void dummy_check_interrupts() {}

static RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Expand: return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual: return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::Original:
        default: return RT64::UserConfiguration::AspectRatio::Original;
    }
}

static RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::None:
        default: return RT64::UserConfiguration::Antialiasing::None;
    }
}

static RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Display: return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual: return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::Original:
        default: return RT64::UserConfiguration::RefreshRate::Original;
    }
}

static RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::On: return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
        default: return RT64::UserConfiguration::InternalColorFormat::Standard;
    }
}

static void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0 / 9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

static ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success: return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI: return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound: return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound: return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    fprintf(stderr, "Unhandled RT64::Application::SetupResult\n");
    std::exit(EXIT_FAILURE);
}

static ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal: return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
        default: return ultramodern::renderer::GraphicsApi::Auto;
    }
}

kig::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    static unsigned char dummy_rom_header[0x40];

    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#else
    appCore.window = window_handle;
#endif
    appCore.checkInterrupts = dummy_check_interrupts;
    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;
    appCore.MI_INTR_REG = &MI_INTR_REG;
    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = developer_mode;
    app->emulatorConfig.dither.postBlendNoiseNegative = true;
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12; break;
        case ultramodern::renderer::GraphicsApi::Vulkan: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan; break;
        case ultramodern::renderer::GraphicsApi::Metal: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal; break;
        case ultramodern::renderer::GraphicsApi::Auto:
        default: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic; break;
    }

    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Present what the VI scans out (Console). RT64's default, SkipBuffering, presents the framebuffer
    // the latest workload drew into, a frame early; KI Gold builds its frames in several passes, so that
    // sometimes showed an unfinished frame (the title logo vanishing for a frame).
    // KIG_PRESENT_MODE=skip|early restores the other modes, for comparison.
    using PresentMode = RT64::EnhancementConfiguration::Presentation::Mode;
    PresentMode present_mode = PresentMode::Console;
    if (const char* m = std::getenv("KIG_PRESENT_MODE")) {
        if (std::string(m) == "skip") present_mode = PresentMode::SkipBuffering;
        if (std::string(m) == "early") present_mode = PresentMode::PresentEarly;
    }
    app->enhancementConfig.presentation.mode = present_mode;
    app->updateEnhancementConfig();
}

kig::renderer::RT64Context::~RT64Context() = default;

// The viewport the last gfx task ended with (see send_dl).
static interop::RSPViewport last_viewport{};
static bool has_last_viewport = false;

void kig::renderer::RT64Context::send_dl(const OSTask* task) {
    kig::stats::display_lists++;
    kig::stats::summarize_dl(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    kig::stats::dump_rdram(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF,
                           pending_rdp_lists.empty() ? 0 : pending_rdp_lists[0].start,
                           pending_rdp_lists.empty() ? 0 : pending_rdp_lists[0].start + (uint32_t)pending_rdp_lists[0].words.size());
    kig::framelog::on_display_list(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    kig_watch(app->core.RDRAM);  // KIG_WATCH (src/game/test_hooks.cpp)
    if (!kig::capture::render_needed()) {
        return;  // fast test run, no capture coming up (kig_capture.h)
    }
    flush_rdp_lists(true);
    RT64::RSP& rsp = *app->state->rsp;
    rsp.reset();
    // F3D keeps the viewport in DMEM, which the next task's ucode data load doesn't cover, so on hardware a
    // task that sets no viewport draws with the previous task's. KI Gold relies on it: near the end of the
    // victory pose it turns its viewport G_MOVEMEM into a no-op, and RT64's reset (all-zero viewport) then
    // collapsed the whole stage to a point, leaving a black floor.
    if (has_last_viewport) {
        rsp.viewportStack[0] = last_viewport;
        rsp.viewportChanged = true;
    }
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    last_viewport = rsp.viewportStack[rsp.viewportStackSize - 1];
    has_last_viewport = true;
    wait_for_rdram_writeback();
}

// The runtime raises the DP interrupt when send_dl returns. On hardware that means the frame's pixels are
// in RDRAM, and KI Gold relies on it: it reads its framebuffers back with the CPU (fades, the bio screens
// save and restore the rendered backdrop). RT64 renders on its own thread and copies framebuffers back to
// RDRAM when that workload finishes, so wait for it. KIG_ASYNC_RDRAM=1 skips the wait (faster, but the
// CPU then reads stale framebuffers).
void kig::renderer::RT64Context::wait_for_rdram_writeback() {
    static const bool async = std::getenv("KIG_ASYNC_RDRAM") != nullptr;
    if (!async) {
        app->workloadQueue->waitForWorkloadId(app->state->workloadId);
    }
}

// KI Gold starts each frame with a CPU-built RDP list (clear, background) and then the frame's gfx task.
// Run separately, the list's closing full sync makes RT64 end a frame that has only the clear in it, and
// that could be presented: black/white flashes, the title logo flickering. So the list is copied (the
// game reuses the buffer) and run just before the next gfx task, without its full sync, as part of the
// same RT64 frame. A list with no gfx task after it runs (with its full sync) before the next present.
void kig::renderer::RT64Context::send_rdp_list(uint32_t start, uint32_t end) {
    kig::stats::rdp_lists++;
    kig::framelog::on_rdp_list(app->core.RDRAM, start, end);
    static const bool skip = std::getenv("KIG_SKIP_RDP_LISTS") != nullptr;  // debugging
    if (skip || !kig::capture::render_needed()) {
        return;
    }
    start &= 0x3FFFFF8;
    end &= 0x3FFFFF8;
    if (end <= start || end - start > 0x10000) {
        return;
    }
    PendingRdpList list;
    list.start = start;
    list.words.assign(app->core.RDRAM + start, app->core.RDRAM + end);
    pending_rdp_lists.push_back(std::move(list));
}

void kig::renderer::RT64Context::flush_rdp_lists(bool drop_final_sync) {
    for (size_t i = 0; i < pending_rdp_lists.size(); i++) {
        PendingRdpList& list = pending_rdp_lists[i];
        size_t size = list.words.size();
        const bool last = i + 1 == pending_rdp_lists.size();
        // RDRAM words are host-endian 32-bit, so the command byte is the top byte of the first word.
        if (drop_final_sync && last && size >= 8 && (reinterpret_cast<const uint32_t*>(list.words.data() + size - 8)[0] >> 24) == 0xE9) {
            size -= 8;
        }
        if (size > 0) {
            app->processDisplayLists(list.words.data(), 0, (uint32_t)size, false);
        }
    }
    pending_rdp_lists.clear();
}

// ultramodern queues this on each VI *before the game starts* (with a dummy VI mode). update_screen
// presents the launcher on its own, so there is nothing to render here.
void kig::renderer::RT64Context::send_dummy_workload(uint32_t) {}

void kig::renderer::RT64Context::update_screen() {
    kig::stats::screen_updates++;
    static uint32_t last_origin = 0;
    uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
    kig::framelog::on_screen_update(origin);
    if (origin != last_origin) {
        last_origin = origin;
        kig::stats::origin_changes++;
    }
    if (!kig::capture::render_needed()) {
        return;
    }
    flush_rdp_lists(false);
    if (kig::ui::visible()) {
        // RT64 presents only when the screen changed; the launcher (no game yet) and the menu (game
        // paused) need a present every VI to draw over.
        app->state->updateScreen(app->core.decodeVI(), true);
    } else {
        app->updateScreen();
    }
}

void kig::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool kig::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                                const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }
    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }
    set_application_user_config(app.get(), new_config);
    app->updateUserConfig(true);
    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void kig::renderer::RT64Context::enable_instant_present() {
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    app->updateEnhancementConfig();
}

uint32_t kig::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float kig::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> kig::renderer::create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    kig::crash::set_rdram(rdram);
    kig::steps::set_rdram(rdram);
    return std::make_unique<kig::renderer::RT64Context>(rdram, window_handle, developer_mode);
}
