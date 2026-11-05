// Save states: see include/kig_savestate.h.
//
// Why this works: ultramodern runs one game thread at a time, and a thread that isn't running is blocked
// in the scheduler. At the top of the idle loop (pause_self) every other game thread is blocked, and a
// thread blocked in a message queue wait can be resumed by calling the OS function again (it rechecks
// the queue, as its wait loop does). All of a thread's MIPS state is in its recomp_context and RDRAM;
// what is native is its call chain, which the shadow stack records (N64Recomp patch 0002). To load a
// state, the idle thread ends every other game thread, restores RDRAM and the native state, and starts
// each saved thread again: its entry function's resume dispatch jumps into the call it was in, and so
// on down the chain, to the OS call it was blocked in, where it parks until the scheduler runs it.
//
// The state file: "KIGSTATE", a version, then the sections below in order (see write_state()).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/savestate.hpp"
#include "librecomp/overlays.hpp"
#include "kig_log.h"
#include "kig_savestate.h"
#include "kig_segments.h"

extern "C" recomp_func_t* get_function(int32_t vram);

namespace {
    namespace us = ultramodern::savestate;

    constexpr char Magic[8] = { 'K', 'I', 'G', 'S', 'T', 'A', 'T', 'E' };
    constexpr uint32_t Version = 1;
    constexpr uint32_t RdramSize = 0x800000;
    // How long a request waits for a safe point before giving up.
    constexpr auto SafePointTimeout = std::chrono::seconds(10);

    // Each game thread's registers and shadow stack, for the idle thread to read while it is blocked.
    struct ThreadData {
        recomp_context* ctx = nullptr;
        RecompShadowStack* ss = nullptr;
    };
    thread_local ThreadData thread_data;

    struct SavedThread {
        int32_t thread = 0;      // OSThread
        int32_t entrypoint = 0;
        int32_t arg = 0;
        uint32_t started = 0;    // 0: created but never started (it waits to be started again)
        recomp_context ctx{};
        std::vector<RecompSSEntry> chain;
    };

    struct SavedOverlay {
        int32_t ram;
        uint32_t size;
        std::string name;
    };

    struct State {
        std::vector<uint8_t> rdram;
        int32_t idle_thread = 0;
        int32_t running_queue = 0;
        us::EventSave events{};
        std::vector<kig::savestate_hooks::TlbEntry> tlb;
        std::vector<recomp::overlays::LoadedSectionSave> sections;
        std::vector<SavedOverlay> overlays;
        uint32_t count_state = 0;
        std::vector<SavedThread> threads;
    };

    enum class Request { None, Save, Load };

    std::mutex mutex;  // guards everything below
    std::atomic<Request> request{ Request::None };
    std::filesystem::path request_path;
    std::shared_ptr<State> load_state;
    std::chrono::steady_clock::time_point request_time;
    kig::savestate::SaveCondition save_condition;  // (read by the idle thread while a save is pending)
    std::string last_wait_reason;
    kig::savestate::Result result;
    std::filesystem::path slot_dir;

    // A loaded state stays alive until its threads have resumed (they read their SavedThread).
    std::shared_ptr<State> resuming_state;

    void finish(bool ok, bool was_load, const std::string& message) {
        std::lock_guard lock{ mutex };
        result.serial++;
        result.ok = ok;
        result.was_load = was_load;
        result.message = message;
        request = Request::None;
        load_state.reset();
        fprintf(stderr, "[kig] save state: %s\n", message.c_str());
        kig::log::flush();
    }

    // ---- file format ----
    struct Writer {
        std::vector<uint8_t> out;
        void bytes(const void* p, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            out.insert(out.end(), b, b + n);
        }
        template <typename T> void pod(const T& v) { bytes(&v, sizeof(T)); }
        void str(const std::string& s) { pod((uint32_t)s.size()); bytes(s.data(), s.size()); }
    };

    struct Reader {
        const std::vector<uint8_t>& in;
        size_t pos = 0;
        bool ok = true;
        void bytes(void* p, size_t n) {
            if (pos + n > in.size()) { ok = false; std::memset(p, 0, n); return; }
            std::memcpy(p, in.data() + pos, n);
            pos += n;
        }
        template <typename T> T pod() { T v{}; bytes(&v, sizeof(T)); return v; }
        std::string str() {
            uint32_t n = pod<uint32_t>();
            if (!ok || pos + n > in.size()) { ok = false; return {}; }
            std::string s(reinterpret_cast<const char*>(in.data() + pos), n);
            pos += n;
            return s;
        }
    };

    std::vector<uint8_t> write_state(const State& s) {
        Writer w;
        w.bytes(Magic, sizeof(Magic));
        w.pod(Version);
        w.pod((uint32_t)sizeof(recomp_context));
        w.pod((uint32_t)s.rdram.size());
        w.bytes(s.rdram.data(), s.rdram.size());
        w.pod(s.idle_thread);
        w.pod(s.running_queue);
        w.pod(s.events);
        w.pod((uint32_t)s.tlb.size());
        for (const auto& e : s.tlb) w.pod(e);
        w.pod((uint32_t)s.sections.size());
        for (const auto& e : s.sections) w.pod(e);
        w.pod((uint32_t)s.overlays.size());
        for (const auto& o : s.overlays) { w.pod(o.ram); w.pod(o.size); w.str(o.name); }
        w.pod(s.count_state);
        w.pod((uint32_t)s.threads.size());
        for (const SavedThread& t : s.threads) {
            w.pod(t.thread); w.pod(t.entrypoint); w.pod(t.arg); w.pod(t.started);
            w.pod(t.ctx);
            w.pod((uint32_t)t.chain.size());
            for (const auto& e : t.chain) w.pod(e);
        }
        return std::move(w.out);
    }

    bool read_state(const std::vector<uint8_t>& data, State& s, std::string& error) {
        Reader r{ data };
        char magic[8];
        r.bytes(magic, sizeof(magic));
        if (!r.ok || std::memcmp(magic, Magic, sizeof(Magic)) != 0) { error = "not a save state"; return false; }
        if (r.pod<uint32_t>() != Version) { error = "save state version differs"; return false; }
        if (r.pod<uint32_t>() != sizeof(recomp_context)) { error = "save state from a different build"; return false; }
        uint32_t rdram_size = r.pod<uint32_t>();
        if (rdram_size != RdramSize) { error = "bad RDRAM size"; return false; }
        s.rdram.resize(rdram_size);
        r.bytes(s.rdram.data(), rdram_size);
        s.idle_thread = r.pod<int32_t>();
        s.running_queue = r.pod<int32_t>();
        s.events = r.pod<us::EventSave>();
        s.tlb.resize(r.pod<uint32_t>());
        for (auto& e : s.tlb) e = r.pod<kig::savestate_hooks::TlbEntry>();
        s.sections.resize(r.pod<uint32_t>());
        for (auto& e : s.sections) e = r.pod<recomp::overlays::LoadedSectionSave>();
        s.overlays.resize(r.pod<uint32_t>());
        for (auto& o : s.overlays) { o.ram = r.pod<int32_t>(); o.size = r.pod<uint32_t>(); o.name = r.str(); }
        s.count_state = r.pod<uint32_t>();
        uint32_t n = r.pod<uint32_t>();
        if (!r.ok || n > 64) { error = "truncated save state"; return false; }
        s.threads.resize(n);
        for (SavedThread& t : s.threads) {
            t.thread = r.pod<int32_t>(); t.entrypoint = r.pod<int32_t>(); t.arg = r.pod<int32_t>();
            t.started = r.pod<uint32_t>();
            t.ctx = r.pod<recomp_context>();
            uint32_t depth = r.pod<uint32_t>();
            if (!r.ok || depth > RECOMP_SS_MAX) { error = "truncated save state"; return false; }
            t.chain.resize(depth);
            for (auto& e : t.chain) e = r.pod<RecompSSEntry>();
        }
        if (!r.ok) { error = "truncated save state"; return false; }
        return true;
    }

    // ---- safe point ----
    // Every other game thread is blocked where it can be resumed, and nothing is in flight.
    bool at_safe_point(uint8_t* rdram, UltraThreadContext* self, std::string& why) {
        if (self == nullptr) { why = "not on a game thread"; return false; }
        if (!us::rsp_idle()) { why = "an RSP task is running"; return false; }
        if (!us::external_queue_empty()) { why = "messages are pending"; return false; }
        for (UltraThreadContext* c : us::live_threads()) {
            if (c == self || !c->started) {
                continue;
            }
            const OSThread* t = TO_PTR(OSThread, c->thread);
            const ThreadData* td = static_cast<const ThreadData*>(c->game_data);
            if (td == nullptr || td->ctx == nullptr) { why = "thread " + std::to_string(t->id) + " has no registers"; return false; }
            if (c->park.load() != 1) { why = "thread " + std::to_string(t->id) + " isn't blocked in a message queue"; return false; }
            if (td->ss->depth == 0 || td->ss->depth > RECOMP_SS_MAX) { why = "thread " + std::to_string(t->id) + "'s call chain can't be saved"; return false; }
        }
        return true;
    }

    std::shared_ptr<State> capture(uint8_t* rdram, UltraThreadContext* self) {
        auto s = std::make_shared<State>();
        s->rdram.assign(rdram, rdram + RdramSize);
        s->idle_thread = self->thread;
        s->running_queue = us::get_running_queue();
        s->events = us::get_events(rdram);
        s->tlb = kig::savestate_hooks::get_tlb();
        s->sections = recomp::overlays::get_loaded_sections();
        for (const auto& o : kig::savestate_hooks::get_overlays()) {
            s->overlays.push_back({ o.ram, o.size, o.name ? o.name : "" });
        }
        s->count_state = kig::savestate_hooks::get_count_state();
        for (UltraThreadContext* c : us::live_threads()) {
            if (c == self) {
                continue;
            }
            SavedThread t;
            t.thread = c->thread;
            t.entrypoint = c->entrypoint;
            t.arg = c->arg;
            t.started = c->started ? 1 : 0;
            if (t.started) {
                const ThreadData* td = static_cast<const ThreadData*>(c->game_data);
                t.ctx = *td->ctx;
                t.chain.assign(td->ss->e, td->ss->e + td->ss->depth);
            }
            s->threads.push_back(std::move(t));
        }
        return s;
    }

    [[noreturn]] void fatal(const std::string& message) {
        fprintf(stderr, "[kig] save state: %s; the game can't continue\n", message.c_str());
        kig::log::flush();
        std::fflush(nullptr);
        std::_Exit(3);
    }

    // On the idle thread at a safe point. Past terminate_other_threads there is no going back.
    void apply(uint8_t* rdram, UltraThreadContext* self, std::shared_ptr<State> s) {
        if (!us::terminate_other_threads(rdram)) {
            fatal("a game thread didn't end");
        }
        std::memcpy(rdram, s->rdram.data(), RdramSize);
        TO_PTR(OSThread, self->thread)->context = self;
        us::set_running_queue(s->running_queue);
        us::set_events(rdram, s->events);
        kig::savestate_hooks::set_tlb(s->tlb);
        recomp::overlays::set_loaded_sections(s->sections);
        std::vector<kig::savestate_hooks::Overlay> overlays;
        for (const SavedOverlay& o : s->overlays) {
            const char* name = nullptr;
            for (const kig::CodeSegment& seg : kig::code_segments) {
                if (o.name == seg.name) name = seg.name;
            }
            overlays.push_back({ o.ram, o.size, name ? name : "?" });
        }
        kig::savestate_hooks::set_overlays(overlays);
        kig::savestate_hooks::set_count_state(s->count_state);
        us::discard_external_messages();

        resuming_state = s;
        recomp_resume_active = 1;
        std::vector<UltraThreadContext*> resumed;
        for (const SavedThread& t : s->threads) {
            UltraThreadContext* c = us::create_restored_thread(rdram, t.thread, t.entrypoint, t.arg, t.started ? &t : nullptr);
            if (t.started) {
                resumed.push_back(c);
            }
        }
        // Each rebuilds its call chain and parks; wait for all of them before the game goes on.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (UltraThreadContext* c : resumed) {
            while (c->park.load() != 1) {
                if (std::chrono::steady_clock::now() > give_up) {
                    fatal("a restored thread didn't reach its blocking call");
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        recomp_resume_active = 0;
    }

    void idle_hook(uint8_t* rdram) {
        const Request req = request.load(std::memory_order_acquire);
        if (req == Request::None) {
            return;
        }
        UltraThreadContext* self = us::this_thread_context(rdram);
        if (req == Request::Save && save_condition.addr != 0) {
            // Waits (without a timeout) until the condition holds; the safe point timeout counts from then.
            const uint32_t p = save_condition.addr & 0x1FFFFFFF;
            uint32_t v = 0;
            for (int i = 0; i < save_condition.bits / 8; i++) v = v << 8 | rdram[(p + i) ^ 3];
            if (v != save_condition.value) {
                std::lock_guard lock{ mutex };
                request_time = std::chrono::steady_clock::now();
                return;
            }
        }
        std::string why;
        if (!at_safe_point(rdram, self, why)) {
            std::lock_guard lock{ mutex };
            last_wait_reason = why;
            if (std::chrono::steady_clock::now() - request_time > SafePointTimeout) {
                std::string message = std::string(req == Request::Save ? "save" : "load") + " failed: no safe point (" + why + ")";
                // (finish takes the mutex)
                request = Request::None;
                load_state.reset();
                result.serial++;
                result.ok = false;
                result.was_load = req == Request::Load;
                result.message = message;
                fprintf(stderr, "[kig] save state: %s\n", message.c_str());
            }
            return;
        }
        std::filesystem::path path;
        std::shared_ptr<State> state;
        {
            std::lock_guard lock{ mutex };
            path = request_path;
            state = load_state;
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (req == Request::Save) {
            std::shared_ptr<State> s = capture(rdram, self);
            std::vector<uint8_t> data = write_state(*s);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
            f.close();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!f) {
                finish(false, false, "couldn't write " + path.string());
            } else {
                finish(true, false, "saved " + path.string() + " (" + std::to_string(s->threads.size()) + " threads, " +
                                         std::to_string(ms) + " ms)");
            }
        } else {
            if (state->idle_thread != self->thread) {
                finish(false, true, "load failed: the state's idle thread differs");
                return;
            }
            apply(rdram, self, state);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            finish(true, true, "loaded " + path.string() + " (" + std::to_string(ms) + " ms)");
        }
    }

    // A restored thread, on its new host thread: rebuild the call chain (recomp_resume_enter does the rest).
    void resume_thread(uint8_t* rdram, UltraThreadContext* c) {
        const SavedThread& t = *static_cast<const SavedThread*>(c->resume);
        recomp_context ctx = t.ctx;
        ctx.f_odd = ctx.mips3_float_mode ? &ctx.f1.u32l : &ctx.f0.u32h;
        RecompShadowStack* ss = recomp_ss_self();
        thread_data = { &ctx, ss };
        c->game_data = &thread_data;
        ss->depth = 0;
        ss->target_pending = 0;
        std::copy(t.chain.begin(), t.chain.end(), ss->e);
        ss->resume_pos = 0;
        ss->resume_len = (uint32_t)t.chain.size();
        recomp_func_t* func = get_function(t.entrypoint);
        func(rdram, &ctx);
    }
}

// ---- called from the recompiled code (include/recomp.h) ----
extern "C" uint32_t recomp_resume_enter(uint8_t* rdram, recomp_context* ctx, uint32_t func_vram) {
    (void)ctx;
    RecompShadowStack* ss = recomp_ss_self();
    if (ss->resume_pos >= ss->resume_len) {
        recomp_resume_fail(rdram, ctx, func_vram);
        return 0;
    }
    const RecompSSEntry e = ss->e[ss->resume_pos++];
    if (ss->resume_pos == ss->resume_len) {
        // The innermost frame: its call is the OS call the thread was blocked in. Park until the
        // scheduler runs this thread, then make that call again.
        ss->resume_len = 0;
        ss->resume_pos = 0;
        us::park_self(rdram);
        return e.site;
    }
    if (e.target != 0) {
        ss->target_pending = 1;
        ss->target = e.target;
    }
    return e.site;
}

extern "C" void recomp_resume_fail(uint8_t* rdram, recomp_context* ctx, uint32_t func_vram) {
    (void)rdram;
    (void)ctx;
    RecompShadowStack* ss = recomp_ss_self();
    const uint32_t pos = ss->resume_pos ? ss->resume_pos - 1 : 0;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "resuming a thread failed in %08X (call site %08X, frame %u of %u)", func_vram,
                  pos < RECOMP_SS_MAX ? ss->e[pos].site : 0, pos + 1, ss->resume_len);
    fatal(buf);
}

// ---- API ----
void kig::savestate::install() {
    us::set_idle_hook(idle_hook);
    us::set_resume_thread(resume_thread);
}

void kig::savestate::on_thread_create(uint8_t* rdram, recomp_context* ctx) {
    UltraThreadContext* c = us::this_thread_context(rdram);
    RecompShadowStack* ss = recomp_ss_self();
    ss->depth = 0;
    thread_data = { ctx, ss };
    if (c != nullptr) {
        c->game_data = &thread_data;
    }
}

void kig::savestate::request_save(const std::filesystem::path& path, const SaveCondition& condition) {
    std::lock_guard lock{ mutex };
    if (request != Request::None) {
        return;
    }
    save_condition = condition;
    request_path = path;
    request_time = std::chrono::steady_clock::now();
    request.store(Request::Save, std::memory_order_release);
}

bool kig::savestate::request_load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    std::string err;
    auto s = std::make_shared<State>();
    if (!f) {
        err = "can't open " + path.string();
    } else {
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        read_state(data, *s, err);
    }
    if (!err.empty()) {
        fprintf(stderr, "[kig] save state: load failed: %s\n", err.c_str());
        std::lock_guard lock{ mutex };
        result.serial++;
        result.ok = false;
        result.was_load = true;
        result.message = "load failed: " + err;
        if (error) *error = err;
        return false;
    }
    std::lock_guard lock{ mutex };
    if (request != Request::None) {
        if (error) *error = "busy";
        return false;
    }
    request_path = path;
    load_state = std::move(s);
    request_time = std::chrono::steady_clock::now();
    request.store(Request::Load, std::memory_order_release);
    return true;
}

bool kig::savestate::busy() {
    return request.load() != Request::None;
}

kig::savestate::Result kig::savestate::last_result() {
    std::lock_guard lock{ mutex };
    return result;
}

void kig::savestate::set_slot_dir(const std::filesystem::path& dir) {
    std::lock_guard lock{ mutex };
    slot_dir = dir;
}

std::filesystem::path kig::savestate::slot_path() {
    std::lock_guard lock{ mutex };
    return slot_dir / "quicksave.kigstate";
}
