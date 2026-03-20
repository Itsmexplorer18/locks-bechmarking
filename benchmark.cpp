
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::duration<double, std::milli>;
static double elapsed_ms(Clock::time_point t) { return Ms(Clock::now()-t).count(); }

#if defined(__GNUC__)
#  define PAUSE() __asm__ volatile("pause")
#else
#  define PAUSE() ((void)0)
#endif

static const int N_THREADS = 4;
struct TASLock {
    std::atomic<bool> flag{false};
    void lock()   { while (flag.exchange(true,  std::memory_order_acquire)) PAUSE(); }
    void unlock() {        flag.store  (false, std::memory_order_release); }
};

struct TTASLock {
    std::atomic<bool> flag{false};
    void lock() {
        for (;;) {
            while (flag.load(std::memory_order_relaxed)) PAUSE();
            bool exp = false;
            if (flag.compare_exchange_weak(exp, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) return;
        }
    }
    void unlock() { flag.store(false, std::memory_order_release); }
};

struct TicketLock {
    std::atomic<int> now_serving{0}, next_num{0};
    void lock() {
        int my = next_num.fetch_add(1, std::memory_order_relaxed);
        while (now_serving.load(std::memory_order_acquire) != my) PAUSE();
    }
    void unlock() { now_serving.fetch_add(1, std::memory_order_release); }
};

struct MCSNode {
    std::atomic<MCSNode*> next{nullptr};
    std::atomic<bool>     locked{false};
};
struct MCSLock {
    std::atomic<MCSNode*> tail{nullptr};
    void lock(MCSNode* nd) {
        nd->next.store(nullptr, std::memory_order_relaxed);
        nd->locked.store(true,  std::memory_order_relaxed);
        MCSNode* prev = tail.exchange(nd, std::memory_order_acq_rel);
        if (prev) {
            prev->next.store(nd, std::memory_order_release);
            while (nd->locked.load(std::memory_order_acquire)) PAUSE();
        }
    }
    void unlock(MCSNode* nd) {
        MCSNode* exp = nd;
        if (!nd->next.load(std::memory_order_relaxed) &&
            tail.compare_exchange_strong(exp, nullptr,
                std::memory_order_release, std::memory_order_relaxed)) return;
        MCSNode* succ;
        while (!(succ = nd->next.load(std::memory_order_acquire))) PAUSE();
        succ->locked.store(false, std::memory_order_release);
    }
};

struct PetersonSeq {
    alignas(64) std::atomic<bool> flag[2];
    alignas(64) std::atomic<int>  victim;
    PetersonSeq() { flag[0]=flag[1]=false; victim=0; }
    void lock(int id) {
        flag[id].store(true, std::memory_order_seq_cst);
        victim.store(id,     std::memory_order_seq_cst);
        int o = 1-id;
        while (flag[o].load(std::memory_order_seq_cst) &&
               victim.load(std::memory_order_seq_cst) == id) PAUSE();
    }
    void unlock(int id) { flag[id].store(false, std::memory_order_seq_cst); }
};

struct PetersonRel {
    alignas(64) std::atomic<bool> flag[2];
    alignas(64) std::atomic<int>  victim;
    PetersonRel() { flag[0]=flag[1]=false; victim=0; }
    void lock(int id) {
        flag[id].store(true, std::memory_order_release);
        victim.store(id,     std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int o = 1-id;
        while (flag[o].load(std::memory_order_acquire) &&
               victim.load(std::memory_order_acquire) == id) PAUSE();
    }
    void unlock(int id) { flag[id].store(false, std::memory_order_release); }
};

struct SenseBarrier {
    std::atomic<int>  count;
    std::atomic<bool> sense{true};
    const int         total;
    explicit SenseBarrier(int n) : count(n), total(n) {}
    void wait(bool& my_sense) {
        my_sense = !my_sense;
        if (count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count.store(total, std::memory_order_release);
            sense.store(my_sense, std::memory_order_release);
        } else {
            while (sense.load(std::memory_order_acquire) != my_sense) PAUSE();
        }
    }
};

struct Result {
    const char* name;
    int         threads;
    long long   iters;
    double      ms;
    long long   count;
    bool        correct;
};

static void print_header() {
    printf("\n%-26s %7s %9s %10s %6s\n",
           "Algorithm", "Threads", "ms", "Mops/s", "OK?");
    for (int i = 0; i < 62; ++i) putchar('-');
    putchar('\n');
}

static void print_row(const Result& r) {
    bool bar = (std::string(r.name).find("barrier") != std::string::npos);
    double ops  = bar ? (double)r.iters : (double)r.threads * r.iters;
    double tput = ops / (r.ms / 1000.0) / 1e6;
    printf("%-26s %7d %9.1f %10.2f %6s\n",
           r.name, r.threads, r.ms, tput,
           r.correct ? "YES" : "NO");
}
struct Ctx {
    long long iters;
    long long counter;
    Ctx(long long it) : iters(it), counter(0) {}
};

static void tas_worker(TASLock* lk, Ctx* ctx) {
    for (long long i = 0; i < ctx->iters; ++i) { lk->lock(); ++ctx->counter; lk->unlock(); }
}
static void ttas_worker(TTASLock* lk, Ctx* ctx) {
    for (long long i = 0; i < ctx->iters; ++i) { lk->lock(); ++ctx->counter; lk->unlock(); }
}
static void ticket_worker(TicketLock* lk, Ctx* ctx) {
    for (long long i = 0; i < ctx->iters; ++i) { lk->lock(); ++ctx->counter; lk->unlock(); }
}
static void mcs_worker(MCSLock* lk, Ctx* ctx) {
    MCSNode node;
    for (long long i = 0; i < ctx->iters; ++i) { lk->lock(&node); ++ctx->counter; lk->unlock(&node); }
}
static void mutex_worker(std::mutex* mtx, Ctx* ctx) {
    for (long long i = 0; i < ctx->iters; ++i) {
        std::lock_guard<std::mutex> lg(*mtx);
        ++ctx->counter;
    }
}

static Result bench_tas(long long iters) {
    TASLock lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) threads.emplace_back(tas_worker, &lk, &ctx);
    for (auto& t : threads) t.join();
    return {"TAS lock", N_THREADS, iters, elapsed_ms(t0), ctx.counter, ctx.counter==N_THREADS*iters};
}

static Result bench_ttas(long long iters) {
    TTASLock lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) threads.emplace_back(ttas_worker, &lk, &ctx);
    for (auto& t : threads) t.join();
    return {"TTAS lock", N_THREADS, iters, elapsed_ms(t0), ctx.counter, ctx.counter==N_THREADS*iters};
}

static Result bench_ticket(long long iters) {
    TicketLock lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) threads.emplace_back(ticket_worker, &lk, &ctx);
    for (auto& t : threads) t.join();
    return {"Ticket lock", N_THREADS, iters, elapsed_ms(t0), ctx.counter, ctx.counter==N_THREADS*iters};
}

static Result bench_mcs(long long iters) {
    MCSLock lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) threads.emplace_back(mcs_worker, &lk, &ctx);
    for (auto& t : threads) t.join();
    return {"MCS lock", N_THREADS, iters, elapsed_ms(t0), ctx.counter, ctx.counter==N_THREADS*iters};
}

static Result bench_peterson_seq(long long iters) {
    PetersonSeq lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::thread t0th([&]{ for(long long i=0;i<iters;++i){lk.lock(0);++ctx.counter;lk.unlock(0);}});
    std::thread t1th([&]{ for(long long i=0;i<iters;++i){lk.lock(1);++ctx.counter;lk.unlock(1);}});
    t0th.join(); t1th.join();
    return {"Peterson lock (seq)", 2, iters, elapsed_ms(t0), ctx.counter, ctx.counter==2*iters};
}

static Result bench_peterson_rel(long long iters) {
    PetersonRel lk; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::thread t0th([&]{ for(long long i=0;i<iters;++i){lk.lock(0);++ctx.counter;lk.unlock(0);}});
    std::thread t1th([&]{ for(long long i=0;i<iters;++i){lk.lock(1);++ctx.counter;lk.unlock(1);}});
    t0th.join(); t1th.join();
    return {"Peterson lock (rel)", 2, iters, elapsed_ms(t0), ctx.counter, ctx.counter==2*iters};
}

static Result bench_sense_barrier(long long iters) {
    SenseBarrier bar(N_THREADS);
    long long counter = 0;
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int id = 0; id < N_THREADS; ++id) {
        threads.emplace_back([&, id]{
            bool my_sense = true;
            for (long long i = 0; i < iters; ++i) {
                if (id == 0) ++counter;
                bar.wait(my_sense);
            }
        });
    }
    for (auto& t : threads) t.join();
    return {"Sense-reversal barrier", N_THREADS, iters, elapsed_ms(t0), counter, counter==iters};
}

static Result bench_mutex(long long iters) {
    std::mutex mtx; Ctx ctx(iters);
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) threads.emplace_back(mutex_worker, &mtx, &ctx);
    for (auto& t : threads) t.join();
    return {"std::mutex (baseline)", N_THREADS, iters, elapsed_ms(t0), ctx.counter, ctx.counter==N_THREADS*iters};
}

int main(int argc, char* argv[]) {
    long long   it   = 1000000LL;
    std::string only = "";

    if (argc > 1) it   = std::stoll(argv[1]);
    if (argc > 2) only = argv[2];

    print_header();

    if (only.empty() || only=="tas")     print_row(bench_tas(it));
    if (only.empty() || only=="ttas")    print_row(bench_ttas(it));
    if (only.empty() || only=="ticket")  print_row(bench_ticket(it));
    if (only.empty() || only=="mcs")     print_row(bench_mcs(it));
    if (only.empty() || only=="petseq")  print_row(bench_peterson_seq(it));
    if (only.empty() || only=="petrel")  print_row(bench_peterson_rel(it));
    if (only.empty() || only=="barrier") print_row(bench_sense_barrier(it));
    if (only.empty() || only=="mutex")   print_row(bench_mutex(it));

    return 0;
}
