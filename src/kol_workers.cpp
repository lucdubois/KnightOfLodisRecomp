// kol_workers.cpp — the row pool and the worker's section timer
// (kol_workers.h).

#include "kol_workers.h"
#include "kol_env.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kol {

RowPool& row_pool() {
    // Never destroyed: its threads are detached and live as long as the app.
    // 2 helpers, on every device. Measured 2026-11-18 on the Mac's scripted
    // pan of map 55 (900 frames; the process's CPU time, the worker's mean
    // job): none 21.0 s / 11.3 ms, two 21.9 s / 6.8 ms, six 26.9 s / 6.6 ms.
    // Two buy 40% of the job for 4% more work; four more buy 3% for a
    // quarter more work (on the Steam Deck, 7%). Six was the desktop's
    // default for a few hours that day.
    static RowPool* pool = new RowPool(
        std::max(0, std::min(7, static_cast<int>(kol::env_num_or("KOL_PERSP_THREADS", 2.0f)))));
    return *pool;
}
int chunk_rows() {
    static const int n = std::max(1, static_cast<int>(kol::env_num_or("KOL_PERSP_CHUNK", 16.0f)));
    return n;
}
int fine_rows() { return std::max(1, chunk_rows() / 4); }

void parallel_rows(int rows, int chunk, const std::function<void(int, int, int)>& fn) {
    row_pool().run(rows, chunk, fn);
}

namespace {
bool worker_time_on() {
    static const bool on = std::getenv("KOL_WORKER_TIME") != nullptr;
    return on;
}
struct WorkerSec { const char* name; double ms; };
WorkerSec g_worker_secs[48];
int g_worker_nsec = 0;
long g_worker_jobs = 0;
long long worker_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void worker_time_add(const char* name, long long ns) {
    for (int i = 0; i < g_worker_nsec; ++i)
        if (std::strcmp(g_worker_secs[i].name, name) == 0) { g_worker_secs[i].ms += ns / 1e6; return; }
    if (g_worker_nsec < 48) g_worker_secs[g_worker_nsec++] = {name, ns / 1e6};
}
}  // namespace
WorkerTimer::WorkerTimer(const char* section) : name(section), t0(worker_time_on() ? worker_now_ns() : 0) {}
WorkerTimer::~WorkerTimer() {
    if (t0) worker_time_add(name, worker_now_ns() - t0);
}
void WorkerTimer::lap(const char* next) {
    if (!t0) return;
    const long long now = worker_now_ns();
    worker_time_add(name, now - t0);
    name = next;
    t0 = now;
}
void worker_time_job_done() {
    if (!worker_time_on()) return;
    if (++g_worker_jobs % 300 != 0) return;
    std::sort(g_worker_secs, g_worker_secs + g_worker_nsec, [](const WorkerSec& a, const WorkerSec& b) { return a.ms > b.ms; });
    double total = 0.0;
    for (int i = 0; i < g_worker_nsec; ++i) total += g_worker_secs[i].ms;
    std::fprintf(stderr, "[worker-time] %ld jobs, mean per job: total %.2f ms:", g_worker_jobs, total / 300.0);
    for (int i = 0; i < g_worker_nsec; ++i)
        std::fprintf(stderr, " %s %.2f", g_worker_secs[i].name, g_worker_secs[i].ms / 300.0);
    std::fprintf(stderr, "\n");
    g_worker_nsec = 0;
}

}  // namespace kol
