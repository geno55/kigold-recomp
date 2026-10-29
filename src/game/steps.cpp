// Step scripts: KIG_STEPS=<file> runs a scenario's steps (tests/scenarios/*.toml, `steps`), one step tick
// per VI, the same unit the reference emulator harness (tools/refemu.py) uses for retro_run. The
// language is documented there; this is the kig side of it. Outputs (SCREENSHOT, DUMP_RAM) go to
// KIG_CAPTURE_DIR. Each executed step is logged ("[kig] step vi=<n> line <l>: ..."), so a run can be
// lined up with the reference's log step by step.
//
// RAM addresses are KSEG0/KSEG1 (0x80000000.../0xA0000000...); RDRAM is 8 MB. DUMP_RAM writes all of it
// big-endian, the reference's layout. SAVE_STATE/LOAD_STATE only exist on the reference: here they are
// skipped with a log line.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "kig_capture.h"
#include "kig_input.h"
#include "kig_steps.h"

namespace {
    constexpr uint32_t RdramSize = 0x800000;
    constexpr uint32_t DefaultPressVis = 9;  // 0.15 s, as KIG_INPUT_SCRIPT's default

    struct Step {
        int line;
        std::string op;
        std::vector<std::string> args;
    };

    struct Held {  // one port's scripted input
        uint16_t buttons = 0;
        bool has_x = false, has_y = false;
        float x = 0.0f, y = 0.0f;
        uint64_t until = 0;  // VI it is released at (0 = until RELEASE)
    };

    std::vector<Step> script;
    size_t pc = 0;
    uint64_t vi = 0;
    uint64_t wait_until = 0;
    uint64_t wait_ram_since = 0;  // VI the current WAIT_RAM started at (timeouts)
    uint64_t end_vi = 0;          // VI the script finishes at, once its last step has run
    uint8_t* rdram = nullptr;
    std::filesystem::path out_dir = ".";
    std::atomic<bool> finished{ false };
    std::atomic<int> result{ 0 };

    std::mutex held_mutex;
    std::vector<Held> held[kig::input::NumPorts];  // presses (with an end) and holds

    uint32_t parse_u32(const std::string& s) {
        return (uint32_t)std::strtoul(s.c_str(), nullptr, 0);
    }
    uint32_t parse_addr(const std::string& s) {
        return (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
    }

    bool phys(uint32_t addr, uint32_t width, uint32_t& out) {
        out = addr & 0x1FFFFFFF;
        return out + width <= RdramSize;
    }
    // RDRAM is kept as host-order 32-bit words (librecomp's MEM_B: byte n is at n ^ 3).
    uint8_t read8(uint32_t p) { return rdram[p ^ 3]; }
    void write8(uint32_t p, uint8_t v) { rdram[p ^ 3] = v; }
    uint32_t read(uint32_t p, int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits / 8; i++) v = v << 8 | read8(p + i);
        return v;
    }

    void fail(const Step& s, const std::string& why) {
        std::fprintf(stderr, "[kig] step FAIL line %d (%s): %s\n", s.line, s.op.c_str(), why.c_str());
        result = 3;
        finished = true;
    }

    // "A+B+X=0.5" (optionally "2>A"): buttons and stick, as KIG_INPUT_SCRIPT specs.
    bool parse_spec(std::string spec, int& port, Held& h) {
        port = 0;
        if (spec.size() > 2 && spec[1] == '>' && spec[0] >= '1' && spec[0] <= '0' + kig::input::NumPorts) {
            port = spec[0] - '1';
            spec = spec.substr(2);
        }
        static const struct { const char* name; uint16_t bit; } names[] = {
            { "A", kig::input::A }, { "B", kig::input::B }, { "Z", kig::input::Z }, { "START", kig::input::Start },
            { "L", kig::input::L }, { "R", kig::input::R }, { "DU", kig::input::DUp }, { "DD", kig::input::DDown },
            { "DL", kig::input::DLeft }, { "DR", kig::input::DRight }, { "CU", kig::input::CUp },
            { "CD", kig::input::CDown }, { "CL", kig::input::CLeft }, { "CR", kig::input::CRight },
        };
        std::stringstream parts{ spec };
        std::string part;
        while (std::getline(parts, part, '+')) {
            if (part.rfind("X=", 0) == 0) { h.has_x = true; h.x = (float)std::atof(part.c_str() + 2); continue; }
            if (part.rfind("Y=", 0) == 0) { h.has_y = true; h.y = (float)std::atof(part.c_str() + 2); continue; }
            bool found = false;
            for (auto& n : names) {
                if (part == n.name) { h.buttons |= n.bit; found = true; }
            }
            if (!found) return false;
        }
        return true;
    }

    void dump_ram(const std::string& label) {
        std::vector<char> be(RdramSize);
        for (uint32_t i = 0; i < RdramSize; i++) be[i] = (char)read8(i);
        std::ofstream f{ out_dir / (label + ".bin"), std::ios::binary };
        f.write(be.data(), be.size());
    }

    // LOAD_RAM <file> <start> <end> [<start> <end> ...]: copies those ranges of a big-endian RDRAM dump (a
    // DUMP_RAM file, e.g. the reference's; relative paths are in KIG_CAPTURE_DIR) into RDRAM, to start both
    // sides from the same game state. It runs on the VI thread, like WRITE_RAM: use it at a quiet point.
    bool load_ram(const std::vector<std::string>& a, std::string& why) {
        if (a.size() < 3 || a.size() % 2 != 1) {
            why = "usage: LOAD_RAM <file> <start> <end> [<start> <end> ...]";
            return false;
        }
        std::ifstream f{ out_dir / a[0], std::ios::binary };
        std::vector<char> be((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (be.size() != RdramSize) {
            why = "can't read an 8 MB dump from " + (out_dir / a[0]).string();
            return false;
        }
        for (size_t i = 1; i < a.size(); i += 2) {
            uint32_t start, end;
            if (!phys(parse_addr(a[i]), 0, start) || !phys(parse_addr(a[i + 1]), 0, end) || end < start) {
                why = "bad range " + a[i] + "-" + a[i + 1];
                return false;
            }
            for (uint32_t p = start; p < end; p++) write8(p, (uint8_t)be[p]);
        }
        return true;
    }

    // Runs steps from pc until one blocks this VI (or the script ends or fails).
    void run() {
        while (pc < script.size() && !finished) {
            const Step& s = script[pc];
            if (vi < wait_until) return;
            auto log = [&] {
                std::fprintf(stderr, "[kig] step vi=%llu t=%.2f line %d: %s", (unsigned long long)vi,
                             kig::input::script_time(), s.line, s.op.c_str());
                for (const std::string& a : s.args) std::fprintf(stderr, " %s", a.c_str());
                std::fprintf(stderr, "\n");
            };
            const auto& a = s.args;
            if (s.op == "WAIT") {
                if (a.size() != 1) return fail(s, "usage: WAIT <vis>");
                wait_until = vi + parse_u32(a[0]);
                pc++;
                continue;
            }
            if (s.op == "WAIT_RAM") {
                // WAIT_RAM <addr> <8|16|32> <==|!=|&> <value> [timeout_vis]
                if (a.size() < 4) return fail(s, "usage: WAIT_RAM <addr> <8|16|32> <==|!=|&> <value> [timeout]");
                const int bits = std::atoi(a[1].c_str());
                uint32_t p;
                if ((bits != 8 && bits != 16 && bits != 32) || !phys(parse_addr(a[0]), bits / 8, p)) {
                    return fail(s, "bad address or width");
                }
                if (wait_ram_since == 0) wait_ram_since = vi + 1;
                const uint32_t got = read(p, bits), want = parse_u32(a[3]);
                const bool ok = a[2] == "==" ? got == want : a[2] == "!=" ? got != want : (got & want) != 0;
                if (!ok) {
                    const uint64_t timeout = a.size() > 4 ? parse_u32(a[4]) : 3600;
                    if (vi + 1 - wait_ram_since >= timeout) {
                        char why[96];
                        std::snprintf(why, sizeof(why), "timed out after %llu VIs (value %X)", (unsigned long long)timeout, got);
                        return fail(s, why);
                    }
                    return;
                }
                wait_ram_since = 0;
                log();
                pc++;
                continue;
            }
            log();
            if (s.op == "PRESS" || s.op == "HOLD") {
                int port;
                Held h;
                if (a.empty() || !parse_spec(a[0], port, h)) return fail(s, "bad input spec");
                if (s.op == "PRESS") h.until = vi + (a.size() > 1 ? parse_u32(a[1]) : DefaultPressVis);
                std::lock_guard lock{ held_mutex };
                held[port].push_back(h);
            } else if (s.op == "RELEASE") {
                std::lock_guard lock{ held_mutex };
                for (auto& p : held) p.clear();
            } else if (s.op == "WRITE_RAM") {
                // WRITE_RAM <addr> <8|16|32> <value>
                const int bits = a.size() == 3 ? std::atoi(a[1].c_str()) : 0;
                uint32_t p;
                if ((bits != 8 && bits != 16 && bits != 32) || !phys(parse_addr(a[0]), bits / 8, p)) {
                    return fail(s, "usage: WRITE_RAM <addr> <8|16|32> <value>");
                }
                const uint32_t v = parse_u32(a[2]);
                for (int i = 0; i < bits / 8; i++) write8(p + i, (uint8_t)(v >> (bits - 8 * (i + 1))));
            } else if (s.op == "SCREENSHOT") {
                if (a.size() != 1) return fail(s, "usage: SCREENSHOT <label>");
                kig::capture::request(a[0]);
            } else if (s.op == "DUMP_RAM") {
                if (a.size() != 1) return fail(s, "usage: DUMP_RAM <label>");
                dump_ram(a[0]);
            } else if (s.op == "LOAD_RAM") {
                std::string why;
                if (!load_ram(a, why)) return fail(s, why);
            } else if (s.op == "SAVE_STATE" || s.op == "LOAD_STATE") {
                std::fprintf(stderr, "[kig] step line %d: %s skipped (reference emulator only)\n", s.line, s.op.c_str());
            } else if (s.op == "EXIT") {
                result = a.empty() ? 0 : std::atoi(a[0].c_str());
                finished = true;
                return;
            } else {
                return fail(s, "unknown step");
            }
            pc++;
        }
        if (pc >= script.size() && !finished) {
            // Finish a VI after the last step, so a SCREENSHOT it requested has been taken (capture::on_vi
            // holds that VI until it is), and not before a final WAIT is over.
            if (end_vi == 0) end_vi = std::max<uint64_t>(vi + 1, wait_until);
            if (vi >= end_vi) {
                std::fprintf(stderr, "[kig] steps: done at vi=%llu\n", (unsigned long long)vi);
                finished = true;
            }
        }
    }
}

void kig::steps::init() {
    const char* path = std::getenv("KIG_STEPS");
    if (!path) return;
    if (const char* dir = std::getenv("KIG_CAPTURE_DIR")) out_dir = dir;
    std::ifstream f{ path };
    if (!f) {
        std::fprintf(stderr, "[kig] steps: can't read %s\n", path);
        result = 3;
        finished = true;
        return;
    }
    std::string text;
    int line = 0;
    std::vector<std::pair<uint32_t, size_t>> repeats;  // open REPEAT blocks: count, first step
    while (std::getline(f, text)) {
        line++;
        text = text.substr(0, text.find('#'));
        std::stringstream ss{ text };
        Step s{ line };
        if (!(ss >> s.op)) continue;
        for (char& c : s.op) c = (char)std::toupper((unsigned char)c);
        for (std::string arg; ss >> arg;) s.args.push_back(arg);
        // REPEAT <n> ... END (nestable): unrolled here, each copy keeping its source line numbers.
        if (s.op == "REPEAT") {
            repeats.emplace_back(s.args.empty() ? 1 : parse_u32(s.args[0]), script.size());
            continue;
        }
        if (s.op == "END") {
            if (repeats.empty()) {
                std::fprintf(stderr, "[kig] steps: line %d: END without REPEAT\n", line);
                result = 3;
                finished = true;
                return;
            }
            auto [count, start] = repeats.back();
            repeats.pop_back();
            const std::vector<Step> body(script.begin() + start, script.end());
            for (uint32_t i = 1; i < count; i++) script.insert(script.end(), body.begin(), body.end());
            continue;
        }
        script.push_back(std::move(s));
    }
    if (!repeats.empty()) {
        std::fprintf(stderr, "[kig] steps: REPEAT without END\n");
        result = 3;
        finished = true;
        return;
    }
    std::fprintf(stderr, "[kig] steps: %zu from %s\n", script.size(), path);
}

bool kig::steps::active() {
    return !script.empty();
}

void kig::steps::set_rdram(uint8_t* r) {
    rdram = r;
}

void kig::steps::on_vi() {
    if (script.empty() || finished || rdram == nullptr) return;
    vi++;
    {
        std::lock_guard lock{ held_mutex };
        for (auto& p : held) {
            std::erase_if(p, [](const Held& h) { return h.until != 0 && vi >= h.until; });
        }
    }
    run();
}

void kig::steps::apply(int port, uint16_t& buttons, float& x, float& y) {
    if (script.empty() || port < 0 || port >= kig::input::NumPorts) return;
    std::lock_guard lock{ held_mutex };
    for (const Held& h : held[port]) {
        buttons |= h.buttons;
        if (h.has_x) x = h.x;
        if (h.has_y) y = h.y;
    }
}

bool kig::steps::done() {
    return finished;
}

int kig::steps::exit_code() {
    return result;
}
