
---

## Description of Algorithms & Challenges Overcome

I have implemented a total of **6 synchronization primitives**, namely: TAS Lock, TTAS Lock, Ticket Lock, MCS Lock, Peterson Lock with Sequential Consistency, Peterson Lock with Released Consistency. In addition to these, the benchmark also uses `std::mutex` from the C++ standard library as a baseline for comparison.

---

### Description of Algorithms

#### 1. TAS Lock
The Test-and-Set (TAS) lock uses `std::atomic<bool>` with the `exchange` operation to atomically attempt to set the lock flag to `true`. If the exchange returns `true`, the lock is already held, and the thread spins — calling `PAUSE()` to hint to the CPU that it is in a spin-wait loop and reduce power draw and contention. Once the exchange succeeds (returns `false`), the thread has acquired the lock. Releasing simply stores `false` with release semantics. Since the thread that just released the lock is likely to still have the cache line warm, TAS behaves like a **LIFO** lock in practice, meaning it can cause starvation under high contention. It exports `lock()` and `unlock()`.

#### 2. TTAS Lock
The Test-and-Test-and-Set (TTAS) lock improves on TAS by first reading the flag with relaxed semantics before attempting the CAS. A thread only calls `compare_exchange_weak` once it observes the lock as free. This avoids repeatedly issuing write operations to the cache line while the lock is held by another thread, which would otherwise cause cache-line invalidation storms across cores. Like TAS, it is essentially **LIFO** and unfair. It exports `lock()` and `unlock()`.

#### 3. Ticket Lock
The Ticket Lock uses two atomic integers: `now_serving` and `next_num`. Each arriving thread atomically increments `next_num` to claim its ticket, then spins until `now_serving` matches its ticket. When done, a thread increments `now_serving` so the next thread in line may proceed. This guarantees **strict FIFO** ordering — every thread is served in the order it arrived — making the Ticket Lock a fair algorithm. It exports `lock()` and `unlock()`.

#### 4. MCS Lock
The Mellor-Crummey-Scott (MCS) lock builds a linked queue of per-thread nodes. A thread appends its own `MCSNode` to the tail of the queue using an atomic exchange and then spins on its **local** node's `locked` field rather than on any shared global variable. When the predecessor finishes, it sets the successor's `locked` flag to `false`, handing the lock off directly. This eliminates the cache-invalidation broadcast that plagues TAS/TTAS-style locks and provides excellent scalability. MCS is **FIFO** and fair. It exports `lock(MCSNode*)` and `unlock(MCSNode*)`, both requiring the caller to supply its own node.

#### 5. Peterson Lock – Sequential Consistency
Peterson's algorithm supports exactly two threads. Each thread raises an interest flag and sets a shared `victim` variable to its own ID, voluntarily yielding priority to the other. It then spins until either the other thread's flag is lowered or it is no longer the victim. This version uses `std::memory_order_seq_cst` for all atomic operations, providing the strongest correctness guarantee at the cost of full memory fence overhead on every operation. It exports `lock(int id)` and `unlock(int id)`.

#### 6. Peterson Lock – Released Consistency
This variant relaxes memory ordering compared to the sequential version. Flag and victim stores use `release` semantics, and reads use `acquire` semantics. A single `std::atomic_thread_fence(seq_cst)` inserted between the stores and the spin loop establishes the necessary ordering guarantee without requiring every individual operation to carry full sequential-consistency cost. This reduces overhead while preserving correctness. It exports `lock(int id)` and `unlock(int id)`.


---

### Challenges Faced and Overcome

The most demanding part of this project was selecting the correct memory ordering for each algorithm. Using orderings that are too relaxed can introduce subtle correctness bugs — threads racing past a barrier, stale reads in Peterson's algorithm, or missed lock handoffs in MCS — all of which are extremely hard to reproduce and diagnose. Using orderings that are unnecessarily strict, on the other hand, eliminates the performance benefit of implementing custom primitives in the first place.

The MCS unlock path was particularly tricky: there is a window between checking whether a successor node exists and reading its pointer, which requires a spin rather than a direct read to handle the case where a thread has claimed the queue position but not yet written its node pointer.


---

## A Brief Discussion of Performance

All algorithms are benchmarked using `std::chrono::high_resolution_clock`. The benchmark runs each lock algorithm across **4 threads** (or 2 threads for Peterson variants) performing **1,000,000 iterations** each, with each iteration acquiring the lock, incrementing a shared counter, and releasing the lock. Throughput is reported in **million operations per second (Mops/s)**. Correctness is verified by checking that the final counter value equals `threads × iters`.

> **Note:** Actual numbers depend on hardware. The table below should be filled in after running the benchmark on your target machine using `./bench <iters>`.

### Observations & Analysis

**TAS vs TTAS:** TAS performs an atomic write (exchange) on every spin iteration, causing the owning core to repeatedly acquire exclusive ownership of the cache line. Every such attempt invalidates copies on other cores, forcing cache misses. TTAS avoids this by first reading the flag in shared mode and only attempting the write once the lock appears free — keeping the cache line in a read-shared state while waiting, which drastically reduces bus traffic under contention.

**Ticket Lock:** The FIFO fairness of the Ticket Lock comes at a cost: every `unlock()` atomically increments `now_serving`, which invalidates the cache line across all waiting threads simultaneously. All waiting threads suffer a cache miss on the same release event. This wave-of-misses pattern worsens as thread count grows, making Ticket Lock less scalable than MCS despite its comparable fairness.

**MCS Lock:** Spinning on a private per-thread node eliminates the broadcast invalidation problem entirely. Lock handoff is point-to-point — the releasing thread writes only to its direct successor's flag. This gives MCS the best cache behavior of all the spin locks at higher thread counts, at the cost of slightly more complex acquire/release logic and caller-managed node allocation.

**Peterson Seq vs Rel:** Sequential consistency enforces a global total order on every atomic operation, typically requiring a full memory fence on each. The released variant replaces per-operation fences with a single `seq_cst` fence, reducing overhead while still establishing the required ordering guarantee. On strongly-ordered architectures like x86, the difference may be small. On weakly-ordered architectures like ARM, the released version should show a more pronounced speedup.

**Sense-Reversal Barrier:** The barrier is not directly comparable to the locks — it synchronizes forward progress across all threads rather than serializing access to a critical section. Only thread 0 increments the counter per iteration, so measured throughput reflects barrier latency, not lock contention. It is expected to show far fewer context switches than a `pthread_barrier_t`, since threads spin rather than deschedule into the kernel.

**std::mutex (baseline):** `std::mutex` is a robust adaptive lock, typically using a futex on Linux. It spins briefly before yielding to the kernel, which works well for long critical sections but incurs syscall overhead for short ones like a single increment. It is expected to have the highest context-switch count of all algorithms tested here.

---

## A Brief Description of Code Organization

The entire project is contained in a **single self-contained C++ source file**. There are no separate headers, sub-directories, or build dependencies beyond a standard C++11 (or later) compiler.

Execution flow is straightforward:
1. `main()` reads up to two optional command-line arguments: the number of iterations per thread (default: 1,000,000) and an optional filter string to run only one specific benchmark (e.g., `./bench 500000 mcs`).
2. Each selected `bench_*()` function constructs its lock or barrier, spawns the appropriate number of threads, joins them after completion, measures elapsed wall-clock time, and returns a `Result` struct.
3. All results are printed as a formatted table by `print_row()`, including correctness verification.

To build and run:
```bash
g++ -O2 -std=c++17 -pthread bench.cpp -o bench
./bench                   # run all, 1M iterations
./bench 500000            # run all, 500K iterations
./bench 1000000 mcs       # run only MCS lock
```

---

## Description of Every File Submitted

This submission consists of a **single C++ source file**.

### `bench.cpp`

This is the sole file in the project. It is organized into the following logical sections:

- **Timing utilities** — `Clock` and `Ms` type aliases plus `elapsed_ms()` provide a clean interface for wall-clock measurement using `std::chrono::high_resolution_clock`.
- **Platform macro** — `PAUSE()` maps to the x86 `pause` instruction on GCC to improve spin-loop efficiency and falls back to a no-op on other compilers.
- **Lock and barrier definitions** — `TASLock`, `TTASLock`, `TicketLock`, `MCSLock` (with helper `MCSNode`), `PetersonSeq`, `PetersonRel`, and `SenseBarrier` are all self-contained structs with their `lock`/`unlock`/`wait` methods defined inline. No external dependencies.
- **Worker functions** — One static function per lock type (`tas_worker`, `ttas_worker`, `ticket_worker`, `mcs_worker`, `mutex_worker`) implements the per-thread benchmark loop. Peterson and barrier benchmarks use inline lambdas instead.
- **Benchmark driver functions** — One `bench_*()` function per algorithm handles thread creation, joining, timing, and result packaging into a `Result` struct.
- **Output formatting** — `print_header()` prints the column header and separator line. `print_row()` formats and prints one result row, computing Mops/s with awareness of whether the result is from a barrier (single-counter) or a lock (per-thread counter) benchmark.
- **`main()`** — Parses command-line arguments and dispatches to each benchmark in order, or to the single named benchmark if a filter is specified.

---


