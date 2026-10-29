// Crash reporter. On an unhandled SEH exception it prints the
// exception, the faulting thread, module+offset and symbol (kig.exe links with /DEBUG, so recompiled
// functions resolve to their MIPS names, e.g. func_80012345), a native stack walk, the N64 address
// for faults inside the RDRAM reservation, and the thread's trace ring (--trace builds).
// dump_all_threads() prints every thread's stack, for hangs (KIG_DUMP_AFTER=<seconds> runs it once).
// The report also goes to logs/crash-<date>-<time>.txt, and a dialog points at it (unless
// dialogs are off, as in --frames test runs). KIG_CRASH_AFTER=<seconds> crashes on purpose, for testing.
// KIG_WRITE_WATCH=<hex N64 address>[,<max hits>[,<from script time>]] logs the native function (and its
// callers) of every write to that RDRAM word, from that time on (debugging: its 4 KB page is made
// read-only, and each write to the page is single-stepped).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#endif

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "kig_crash.h"
#include "kig_input.h"
#include "kig_log.h"
#include "kig_trace.h"

namespace {
    uint8_t* rdram_base = nullptr;
    std::atomic<bool> dialogs{ true };
    // librecomp reserves 4GB of address space for RDRAM (MEM macros index rdram + (addr - 0x80000000)).
    constexpr uint64_t RdramReserve = 0x100000000ULL;
}

static void arm_write_watch();

void kig::crash::set_rdram(uint8_t* rdram) {
    rdram_base = rdram;
    arm_write_watch();
}

void kig::crash::set_dialogs(bool enabled) {
    dialogs = enabled;
}

#ifdef _WIN32
namespace {
    std::mutex dbghelp_mutex;
    bool sym_initialized = false;

    const char* code_name(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
            case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
            case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
            case 0xE06D7363: return "C++ exception";
            default: return "unknown";
        }
    }

    void init_symbols(HANDLE process) {
        if (!sym_initialized) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(process, nullptr, TRUE);
            sym_initialized = true;
        }
    }

    void describe_address(FILE* out, HANDLE process, uint64_t addr) {
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &module)) {
            GetModuleFileNameA(module, module_path, sizeof(module_path));
        }
        const char* module_name = module_path;
        for (const char* p = module_path; *p; p++) {
            if (*p == '\\' || *p == '/') module_name = p + 1;
        }

        alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::fprintf(out, "0x%016" PRIx64 "  %s+0x%" PRIx64, addr, module_name,
                     module ? addr - reinterpret_cast<uint64_t>(module) : 0);
        if (SymFromAddr(process, addr, &disp, sym)) {
            std::fprintf(out, "  %s+0x%" PRIx64, sym->Name, static_cast<uint64_t>(disp));
        }
        std::fputc('\n', out);
    }

    void walk_stack(FILE* out, HANDLE process, HANDLE thread, CONTEXT walk_ctx, int max_frames) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = walk_ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = walk_ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = walk_ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < max_frames; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walk_ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
                break;
            }
            std::fprintf(out, "  #%02d ", i);
            describe_address(out, process, frame.AddrPC.Offset);
        }
    }

    void print_thread_name(FILE* out, HANDLE thread) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &desc)) && desc) {
            if (desc[0]) std::fprintf(out, " \"%ls\"", desc);
            LocalFree(desc);
        }
    }

    void write_report(FILE* out, EXCEPTION_POINTERS* info) {
        HANDLE process = GetCurrentProcess();
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;
        CONTEXT* ctx = info->ContextRecord;

        std::fprintf(out, "\n[kig] ==== CRASH ====\n");
        std::fprintf(out, "[kig] exception 0x%08lX (%s) on thread %lu", rec->ExceptionCode, code_name(rec->ExceptionCode),
                     GetCurrentThreadId());
        print_thread_name(out, GetCurrentThread());
        std::fputc('\n', out);

        std::fprintf(out, "[kig] at ");
        describe_address(out, process, reinterpret_cast<uint64_t>(rec->ExceptionAddress));

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            uint64_t target = rec->ExceptionInformation[1];
            const char* kind = rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "execute";
            std::fprintf(out, "[kig] %s of 0x%016" PRIx64, kind, target);
            uint64_t base = reinterpret_cast<uint64_t>(rdram_base);
            if (rdram_base && target >= base && target < base + RdramReserve) {
                std::fprintf(out, "  = N64 address 0x%08" PRIX64 " (rdram offset 0x%" PRIX64 ")",
                             (target - base + 0x80000000ULL) & 0xFFFFFFFFULL, target - base);
            }
            std::fputc('\n', out);
        }

        std::fprintf(out, "[kig] rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
        std::fprintf(out, "[kig] rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
        std::fprintf(out, "[kig] r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", ctx->R8, ctx->R9, ctx->R10, ctx->R11);
        std::fprintf(out, "[kig] r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n", ctx->R12, ctx->R13, ctx->R14, ctx->R15);
        if (rdram_base) {
            std::fprintf(out, "[kig] rdram base=%p\n", static_cast<void*>(rdram_base));
        }

        // Native stack walk (works for optimized code via the x64 unwind tables).
        std::fprintf(out, "[kig] stack:\n");
        walk_stack(out, process, GetCurrentThread(), *ctx, 48);

        kig::trace::dump_current_thread(out);
        std::fprintf(out, "[kig] ==== END CRASH ====\n");
        std::fflush(out);
    }

    // logs/crash-YYYYMMDD-HHMMSS.txt, or empty if the log folder isn't set up.
    std::filesystem::path crash_file_path() {
        std::filesystem::path dir = kig::log::dir();
        if (dir.empty()) {
            return {};
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"crash-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return dir / name;
    }

    LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
        static volatile LONG entered = 0;
        if (InterlockedExchange(&entered, 1) != 0) {
            // Another thread is already reporting; let it finish.
            Sleep(INFINITE);
        }
        init_symbols(GetCurrentProcess());
        // The crash file first: it doesn't depend on the log pump threads still running.
        std::filesystem::path crash_path = crash_file_path();
        FILE* crash_file = crash_path.empty() ? nullptr : _wfopen(crash_path.c_str(), L"w");
        if (crash_file != nullptr) {
            write_report(crash_file, info);
            std::fclose(crash_file);
        }
        write_report(stderr, info);
        if (crash_file != nullptr) {
            std::fprintf(stderr, "[kig] crash report saved to %s\n", crash_path.string().c_str());
        }
        kig::log::flush();
        if (dialogs) {
            std::wstring msg = L"Killer Instinct Gold: Recompiled crashed.";
            if (crash_file != nullptr) {
                msg += L"\n\nA report was saved to:\n" + crash_path.wstring();
            }
            MessageBoxW(nullptr, msg.c_str(), L"Killer Instinct Gold: Recompiled", MB_OK | MB_ICONERROR | MB_TOPMOST);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

// KIG_WRITE_WATCH (see the top of the file).
namespace {
    uint8_t* watch_page = nullptr;
    uint8_t* watch_target = nullptr;
    int watch_hits_left = 0;
    double watch_from = 0;  // script time hits start to count from
    thread_local bool watch_stepping = false;
    std::mutex watch_mutex;

    LONG WINAPI write_watch_handler(EXCEPTION_POINTERS* info) {
        EXCEPTION_RECORD* rec = info->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_stepping) {
            watch_stepping = false;
            DWORD old;
            VirtualProtect(watch_page, 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 1) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        uint8_t* target = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
        if (target < watch_page || target >= watch_page + 0x1000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // RDRAM words are host-order: the N64 word's 4 bytes are the 4 host bytes at the same aligned offset.
        if (target >= watch_target && target < watch_target + 4) {
            std::lock_guard lock{ watch_mutex };
            if (watch_hits_left > 0 && kig::input::script_time() >= watch_from) {
                watch_hits_left--;
                HANDLE process = GetCurrentProcess();
                std::lock_guard sym_lock{ dbghelp_mutex };
                init_symbols(process);
                std::fprintf(stderr, "[kig] write watch t=%.2f: %08X written by ", kig::input::script_time(),
                             static_cast<uint32_t>(watch_target - rdram_base + 0x80000000ULL));
                describe_address(stderr, process, reinterpret_cast<uint64_t>(rec->ExceptionAddress));
                walk_stack(stderr, process, GetCurrentThread(), *info->ContextRecord, 6);
                kig::trace::dump_current_thread(stderr);  // trace builds with KIG_TRACE=1: the calls leading here
            }
        }
        DWORD old;
        VirtualProtect(watch_page, 0x1000, PAGE_READWRITE, &old);
        watch_stepping = true;
        info->ContextRecord->EFlags |= 0x100;  // trap flag: re-protect after this one instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

static void arm_write_watch() {
    const char* spec = std::getenv("KIG_WRITE_WATCH");
    if (!spec || !rdram_base) return;
    const uint32_t addr = static_cast<uint32_t>(std::strtoul(spec, nullptr, 16)) & ~3u;
    const char* comma = std::strchr(spec, ',');
    watch_hits_left = comma ? std::atoi(comma + 1) : 50;
    const char* comma2 = comma ? std::strchr(comma + 1, ',') : nullptr;
    watch_from = comma2 ? std::atof(comma2 + 1) : 0.0;
    watch_target = rdram_base + (addr - 0x80000000u);
    watch_page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(watch_target) & ~uintptr_t{ 0xFFF });
    AddVectoredExceptionHandler(1, write_watch_handler);
    DWORD old;
    const bool ok = VirtualProtect(watch_page, 0x1000, PAGE_READONLY, &old) != 0;
    std::fprintf(stderr, "[kig] write watch on %08X (%d hits)%s\n", addr, watch_hits_left, ok ? "" : " FAILED");
}

void kig::crash::dump_all_threads(FILE* out) {
    std::lock_guard lock{ dbghelp_mutex };
    HANDLE process = GetCurrentProcess();
    init_symbols(process);
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    std::fprintf(out, "\n[kig] ==== THREAD DUMP ====\n");
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   te.th32ThreadID);
        if (!thread) {
            continue;
        }
        std::fprintf(out, "[kig] thread %lu", te.th32ThreadID);
        print_thread_name(out, thread);
        std::fputc('\n', out);
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &ctx)) {
                walk_stack(out, process, thread, ctx, 24);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snap);
    std::fprintf(out, "[kig] ==== END THREAD DUMP ====\n");
    std::fflush(out);
}

void kig::crash::install() {
    SetUnhandledExceptionFilter(unhandled_filter);
    // Report instead of showing the Windows Error Reporting dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (const char* after = std::getenv("KIG_DUMP_AFTER")) {
        int seconds = std::atoi(after);
        std::thread{ [seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            kig::crash::dump_all_threads(stderr);
        } }.detach();
    }
    if (const char* after = std::getenv("KIG_CRASH_AFTER")) {
        int seconds = std::atoi(after);
        std::thread{ [seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::fprintf(stderr, "[kig] KIG_CRASH_AFTER: crashing on purpose\n");
            *static_cast<volatile int*>(nullptr) = 0;
        } }.detach();
    }
}
#else
static void arm_write_watch() {}
void kig::crash::install() {}
void kig::crash::set_dialogs(bool) {}
void kig::crash::dump_all_threads(FILE*) {}
#endif
