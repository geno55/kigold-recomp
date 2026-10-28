#ifndef KIG_CAPTURE_H
#define KIG_CAPTURE_H

#include <string>

struct SDL_Window;

namespace kig::capture {
    // Reads KIG_CAPTURE / KIG_CAPTURE_DIR (src/main/capture.cpp). Call after kig::input::init().
    void init();
    // Takes any captures that are due. Main thread, once per main-loop iteration.
    void update(SDL_Window* window);
    // Fast test runs (--speed): render only shortly before each pending capture. Game logic doesn't
    // depend on rendering, and RT64 is the bottleneck at speed. Off by default; no-op without captures.
    void set_render_only_for_captures(bool enabled);
    // Whether display lists and screen updates should be rendered now (gfx thread).
    bool render_needed();
    // Any thread: a screenshot to KIG_CAPTURE_DIR/<label>.bmp now (step scripts). The next on_vi holds the
    // game until the main thread has taken it.
    void request(const std::string& label);
    // VI thread, after each VI (via kig::input::on_vi). In fast runs, holds the game on a capture's VI
    // until the main thread has taken it, so the capture shows exactly that game time.
    void on_vi();
}

#endif
