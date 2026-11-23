// kol_crash.cpp — the session's log and the crash report in it (kol_crash.h).
//
// A crash handler runs in a broken process: it writes with the system's own
// calls only (write / WriteFile), formats numbers itself, and takes no lock.
// After its report it hands the crash back to what was there before (the
// system's default, Android's debuggerd), so the system's report and the
// exit status are as they were.

#include "kol_crash.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "kol_cheevos.h"
#include "kol_dev.h"
#include "runtime.h"
#include "runtime_arm.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <csignal>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

#ifndef KOL_VERSION
#define KOL_VERSION "dev"
#endif

namespace kol {
namespace {

std::string g_dir;
bool g_prev_crashed = false;
std::atomic<int> g_in_crash{0};
std::atomic<long long> g_written{0};
constexpr long long kLogCap = 64ll << 20;   // the session's output kept to 64 MB (a crash report always)

#if defined(_WIN32)
HANDLE g_log = INVALID_HANDLE_VALUE;
void raw_write(const char* p, std::size_t n) {
    HANDLE h = g_log != INVALID_HANDLE_VALUE ? g_log : ::GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || !h) return;
    DWORD done = 0;
    ::WriteFile(h, p, static_cast<DWORD>(n), &done, nullptr);
}
#else
int g_fd = -1;
void raw_write(const char* p, std::size_t n) {
    const int fd = g_fd >= 0 ? g_fd : 2;
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}
#endif

void put(const char* s) { raw_write(s, std::strlen(s)); }
void put_hex(std::uint64_t v) {
    char b[19] = "0x";
    char t[16];
    int n = 0;
    do {
        t[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < 16);
    for (int i = 0; i < n; ++i) b[2 + i] = t[n - 1 - i];
    raw_write(b, static_cast<std::size_t>(2 + n));
}
void put_dec(std::uint64_t v) {
    char t[24];
    int n = 0;
    do {
        t[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v && n < 24);
    char b[24];
    for (int i = 0; i < n; ++i) b[i] = t[n - 1 - i];
    raw_write(b, static_cast<std::size_t>(n));
}

// What the game was at: the guest's PC (as last written back) and the last
// frame presented.
void put_guest() {
    put("guest: pc ");
    put_hex(g_cpu.R[15]);
    put(", frame ");
    put_dec(gbarecomp::g_runtime_presented_frame);
    put("\n");
}

const char* platform() {
#if defined(__ANDROID__)
    return "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

bool file_has(const std::string& path, const char* needle) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.find(needle) != std::string::npos;
}

#if defined(_WIN32)

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// An address as module+offset (no symbols in a release build: the offset
// finds it in the build's own map).
void put_where(const void* addr) {
    put_hex(reinterpret_cast<std::uintptr_t>(addr));
    HMODULE m = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(addr), &m) && m) {
        char name[MAX_PATH] = {0};
        if (::GetModuleFileNameA(m, name, MAX_PATH)) {
            const char* base = std::strrchr(name, '\\');
            put(" (");
            put(base ? base + 1 : name);
            put("+");
            put_hex(reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(m));
            put(")");
        }
    }
}

void put_backtrace() {
    void* frames[62];
    const USHORT n = ::RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    put("backtrace:\n");
    for (USHORT i = 0; i < n; ++i) {
        put("  #");
        put_dec(i);
        put(" ");
        put_where(frames[i]);
        put("\n");
    }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    if (!g_in_crash.exchange(1)) {
        put("\n*** crash: exception ");
        put_hex(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
        put(" at ");
        put_where(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            put(", ");
            put(ep->ExceptionRecord->ExceptionInformation[0] ? "writing " : "reading ");
            put_hex(ep->ExceptionRecord->ExceptionInformation[1]);
        }
        put("\n");
        put_guest();
        put_backtrace();
        put("*** end of the crash report\n");
        if (g_log != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_log);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // (Windows' own report as before)
}

void on_abort(int) {
    if (!g_in_crash.exchange(1)) {
        put("\n*** crash: abort\n");
        put_guest();
        put_backtrace();
        put("*** end of the crash report\n");
        if (g_log != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_log);
    }
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

#else   // POSIX

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "?";
    }
}

std::uintptr_t pc_of(void* ucv) {
    if (!ucv) return 0;
    auto* uc = static_cast<ucontext_t*>(ucv);
#if defined(__APPLE__) && defined(__aarch64__)
    return uc->uc_mcontext ? static_cast<std::uintptr_t>(uc->uc_mcontext->__ss.__pc) : 0;
#elif defined(__APPLE__) && defined(__x86_64__)
    return uc->uc_mcontext ? static_cast<std::uintptr_t>(uc->uc_mcontext->__ss.__rip) : 0;
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#elif defined(__linux__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    (void)uc;
    return 0;
#endif
}

// An address with its library and offset, and the nearest symbol when the
// library exports one.
void put_where(std::uintptr_t addr) {
    put_hex(addr);
    Dl_info info{};
    if (addr && ::dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_fname) {
        const char* base = std::strrchr(info.dli_fname, '/');
        put(" (");
        put(base ? base + 1 : info.dli_fname);
        put("+");
        put_hex(addr - reinterpret_cast<std::uintptr_t>(info.dli_fbase));
        if (info.dli_sname) {
            put(" ");
            put(info.dli_sname);
        }
        put(")");
    }
}

#if defined(__ANDROID__)
struct Trace {
    std::uintptr_t pcs[48];
    int n = 0;
};
_Unwind_Reason_Code trace_one(_Unwind_Context* ctx, void* arg) {
    auto* t = static_cast<Trace*>(arg);
    const std::uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && t->n < 48) t->pcs[t->n++] = pc;
    return t->n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void put_backtrace() {
    Trace t;
    _Unwind_Backtrace(&trace_one, &t);
    put("backtrace:\n");
    for (int i = 0; i < t.n; ++i) {
        put("  #");
        put_dec(static_cast<std::uint64_t>(i));
        put(" ");
        put_where(t.pcs[i]);
        put("\n");
    }
}
#else
void put_backtrace() {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    put("backtrace:\n");
    ::backtrace_symbols_fd(frames, n, g_fd >= 0 ? g_fd : 2);
}
#endif

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_old[NSIG];

void on_signal(int sig, siginfo_t* si, void* ucv) {
    if (!g_in_crash.exchange(1)) {
        put("\n*** crash: signal ");
        put_dec(static_cast<std::uint64_t>(sig));
        put(" (");
        put(signal_name(sig));
        put(")");
        if (sig != SIGABRT && si) {
            put(", address ");
            put_hex(reinterpret_cast<std::uintptr_t>(si->si_addr));
        }
        put("\npc ");
        put_where(pc_of(ucv));
        put("\n");
        put_guest();
        put_backtrace();
        put("*** end of the crash report\n");
        if (g_fd >= 0) ::fsync(g_fd);
#if defined(__ANDROID__)
        __android_log_write(ANDROID_LOG_FATAL, "KoL", "crash: report written to files/logs/last_run.log");
#endif
    }
    // Back to what was there (the default, Android's debuggerd), and again.
    ::sigaction(sig, &g_old[sig], nullptr);
    ::raise(sig);
}

#endif

}  // namespace

void crash_log_begin(const std::string& logs_dir, bool redirect_stdio) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(logs_dir, ec);
    g_dir = logs_dir;
    const std::string last = logs_dir + "/last_run.log", prev = logs_dir + "/previous_run.log";
    if (fs::exists(last, ec)) {
        g_prev_crashed = file_has(last, "*** crash");
        fs::remove(prev, ec);
        fs::rename(last, prev, ec);
    }
    if (std::FILE* f = std::fopen(last.c_str(), "w")) std::fclose(f);
    if (redirect_stdio) {
        // Both appending to the one file, so their lines interleave as they
        // come; unbuffered, so a crash loses nothing (Windows' C runtime
        // buffers fully what it calls line-buffered).
        if (std::freopen(last.c_str(), "a", stdout)) std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (std::freopen(last.c_str(), "a", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#if defined(_WIN32)
    g_log = ::CreateFileW(wide(last).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    g_fd = ::open(last.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
#endif
    char head[200];
    const std::time_t now = std::time(nullptr);
    char when[40] = "?";
    if (const std::tm* tm = std::localtime(&now)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", tm);
    std::snprintf(head, sizeof head, "Knight of Lodis Recomp %s (%s), started %s\n", KOL_VERSION, platform(), when);
    crash_log_write(head, std::strlen(head));
}

void crash_log_write(const char* text, std::size_t n) {
    if (!text || n == 0) return;
#if defined(_WIN32)
    if (g_log == INVALID_HANDLE_VALUE) return;
#else
    if (g_fd < 0) return;
#endif
    if (g_written.fetch_add(static_cast<long long>(n)) > kLogCap) return;
    raw_write(text, n);
}

void crash_handlers_install() {
    static bool done = false;
    if (done) return;
    done = true;
#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(&on_exception);
    std::signal(SIGABRT, &on_abort);
#else
    // Its own stack: a crash from running out of stack still gets its report
    // (on this thread; the others run it on theirs).
    static char alt[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt;
    ss.ss_size = sizeof alt;
    ::sigaltstack(&ss, nullptr);
    for (int sig : kSignals) {
        struct sigaction sa {};
        sa.sa_sigaction = &on_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        ::sigaction(sig, &sa, &g_old[sig]);
    }
#endif
    // An exception nothing caught: its message first (then abort's report).
    std::set_terminate([] {
        put("\n*** crash: std::terminate");
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                put(": ");
                put(x.what());
            } catch (...) {
                put(": an exception that is not std::exception");
            }
        }
        put("\n");
        std::abort();
    });
}

std::string crash_logs_dir() { return g_dir; }

bool crash_log_previous_crashed() { return g_prev_crashed; }

void crash_notice_frame(unsigned long long frame_count) {
    // KOL_CRASH_TEST=<frame> (dev): a crash on purpose at that frame, to
    // check the report (a write through a null pointer).
    if constexpr (kDevTools) {
        static const unsigned long long at = [] {
            const char* e = std::getenv("KOL_CRASH_TEST");
            return e && *e ? std::strtoull(e, nullptr, 10) : 0ull;
        }();
        if (at && frame_count >= at) {
            volatile int* volatile p = nullptr;
            *p = 1;
        }
    }
    static bool done = false;
    if (done || !g_prev_crashed) return;
    static const unsigned long long first = frame_count;
    if (frame_count - first < 90) return;
#if defined(__ANDROID__)
    const char* where = "Its log: Files app, the game's logs.";
#else
    const char* where = "Its log: Options > Technical > Logs.";
#endif
    if (cheevos_notice("The game closed unexpectedly", where) || frame_count - first > 600) done = true;
}

}  // namespace kol
