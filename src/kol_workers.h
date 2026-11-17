#pragma once

// kol_workers.h — the present worker's helpers: the row pool that the warp
// (kol_perspective) and the scene build (kol_focus) split their passes over,
// and the worker's section timer (KOL_WORKER_TIME).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace kol {

// The passes that go row by row are split into chunks of rows, taken by a
// few worker threads and the calling thread alike (a chunk at a time, so a
// worker on a slower core just takes fewer). The guest thread is the one
// that is short of time; the device's other cores are mostly idle. Each row
// is computed exactly as it would be alone, so the result does not depend on
// the split. KOL_PERSP_THREADS: workers besides the caller (default 2).
class RowPool {
public:
    using Fn = std::function<void(int chunk, int begin, int end)>;
    explicit RowPool(int workers) : workers_(workers) {
        for (int t = 0; t < workers; ++t) std::thread([this] { loop(); }).detach();
    }
    // fn over [0, rows) in chunks of chunk_rows; returns once all are done.
    void run(int rows, int chunk_rows, const Fn& fn) {
        // One run at a time (the scene build and the warp share the pool).
        std::lock_guard<std::mutex> one(run_m_);
        const int chunks = (rows + chunk_rows - 1) / chunk_rows;
        if (workers_ == 0 || chunks <= 1) {
            for (int c = 0; c < chunks; ++c)
                fn(c, c * chunk_rows, std::min(rows, (c + 1) * chunk_rows));
            return;
        }
        std::uint64_t gen;
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn;
            rows_ = rows;
            chunk_rows_ = chunk_rows;
            chunks_ = chunks;
            done_ = 0;
            gen = ++gen_;
            // A claim carries the run it belongs to, so a worker that wakes
            // after this run is over cannot take a chunk of it.
            next_.store(gen << 32);
        }
        wake_.notify_all();
        work(gen, fn, rows, chunk_rows, chunks);
        std::unique_lock<std::mutex> lk(m_);
        all_done_.wait(lk, [&] { return done_ == chunks; });
    }

private:
    void work(std::uint64_t gen, const Fn& fn, int rows, int chunk_rows, int chunks) {
        for (;;) {
            std::uint64_t v = next_.load();
            int c;
            do {
                if ((v >> 32) != gen) return;
                c = static_cast<int>(v & 0xFFFFFFFFu);
                if (c >= chunks) return;
            } while (!next_.compare_exchange_weak(v, v + 1));
            fn(c, c * chunk_rows, std::min(rows, (c + 1) * chunk_rows));
            std::lock_guard<std::mutex> lk(m_);
            if (++done_ == chunks) all_done_.notify_one();
        }
    }
    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            const Fn* fn;
            int rows, chunk_rows, chunks;
            {
                std::unique_lock<std::mutex> lk(m_);
                wake_.wait(lk, [&] { return gen_ != seen; });
                seen = gen_;
                fn = fn_;
                rows = rows_;
                chunk_rows = chunk_rows_;
                chunks = chunks_;
            }
            work(seen, *fn, rows, chunk_rows, chunks);
        }
    }
    const int workers_;
    std::mutex run_m_;
    std::mutex m_;
    std::condition_variable wake_, all_done_;
    const Fn* fn_ = nullptr;
    int rows_ = 0, chunk_rows_ = 1, chunks_ = 0, done_ = 0;
    std::uint64_t gen_ = 0;
    std::atomic<std::uint64_t> next_{0};
};

// The pool (KOL_PERSP_THREADS helpers), made on first use.
RowPool& row_pool();
// Rows a chunk, 16; KOL_PERSP_CHUNK=<n> changes it (a split that changes
// the picture is a bug: every row must come out as if computed alone).
int chunk_rows();
// A quarter of that (4), for the passes whose work lies in a band of rows
// (the water, the puddles and their rays): in 16-row chunks a few threads
// took the band and the rest waited.
int fine_rows();

// The warp's row pool (KOL_PERSP_THREADS helpers), for the scene build:
// fn(chunk, begin, end) over [0, rows) in chunks; returns when all are done.
// fn must write only its own rows' results (and read no static thread_local
// the caller filled: each helper has its own).
void parallel_rows(int rows, int chunk, const std::function<void(int, int, int)>& fn);

// KOL_WORKER_TIME=1: the present worker's time by section (kol_focus's
// scene build, kol_perspective's warp), the means over each 300 jobs printed
// then. A WorkerTimer times sections from its construction: lap() ends one
// and starts the next, its destruction ends the last.
struct WorkerTimer {
    explicit WorkerTimer(const char* section);
    ~WorkerTimer();
    void lap(const char* next);
    const char* name;
    long long t0;   // steady clock, ns
};
void worker_time_job_done();   // once a job: counts them, prints at 300

}  // namespace kol
