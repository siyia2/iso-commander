// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREADPOOL_H
#define THREADPOOL_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/core/concurrency.h"
#include "isocmd/threadpool/lockFreeQueue.h"
#include "isocmd/threadpool/moveOnlyTask.h"

/**
 * @file thread_pool.hpp
 * @brief Lock-free thread pool with type-erased move-only tasks.
 *
 * Provides four cooperative components:
 *  - @ref MoveOnlyTask       – a small-buffer-optimised, move-only type-erased callable.
 *  - @ref HazardPointerDomain – a process-wide hazard-pointer registry providing safe
 *                                deferred reclamation for lock-free node-based structures.
 *  - @ref LockFreeQueue      – a Michael–Scott lock-free FIFO queue built on raw
 *                                `atomic<Node*>`, reclaiming retired nodes via hazard pointers.
 *  - @ref ThreadPool         – a fully lock-free worker-thread pool built on the two above.
 *
 * @note Requires C++20 (`std::atomic::wait` / `notify_*`).
 *
 * @par Revision notes (this revision)
 * `LockFreeQueue` previously stored its nodes behind `std::atomic<std::shared_ptr<Node>>`.
 * That compiles and is correct, but on every mainstream libstdc++/libc++ build as of
 * this writing, `atomic<shared_ptr<T>>` is **not** lock-free — `is_lock_free()` and
 * the compile-time `is_always_lock_free` both report `false`, because the
 * implementation protects the pointer+control-block pair with an internal spinlock
 * table keyed by address. That meant every `load`/`store`/`compare_exchange` on
 * `head`, `tail`, or any node's `next` silently took an internal lock — worse, a
 * lock *shared across every unrelated `atomic<shared_ptr<T>>` in the process* that
 * happens to hash to the same table slot — while still paying the cost of
 * heap-allocated control blocks and atomic refcount churn per node.
 *
 * This revision replaces that with the standard fix for the underlying problem
 * (safe reclamation of a node another thread might still be dereferencing) without
 * giving up lock-freedom to get it: **hazard pointers** (Maged Michael, 2004).
 * Nodes are now linked via plain `std::atomic<Node*>`, which *is* natively
 * lock-free CAS on every mainstream 64-bit target. Before a thread dereferences a
 * node it doesn't yet own, it publishes that node's address into a hazard-pointer
 * slot; any other thread wanting to actually `delete` a retired node first scans
 * every live hazard-pointer slot in the process and defers deletion of any node
 * still published there. This removes both the false-sharing/global-lock-table
 * problem and the per-node control-block allocation entirely.
 *
 * The `ThreadPool`'s own `shutdown()` fix from the previous revision (folding the
 * stop flag into `task_state` instead of clobbering it with placeholder wakeup
 * values) is unchanged and still applies — see the `ThreadPool` class doc below.
 */

/**
 * @class ThreadPool
 * @brief Fully lock-free thread pool backed by C++20 atomic wait/notify.
 *
 * Manages a fixed set of worker threads that drain tasks from a @ref LockFreeQueue.
 * Task submission is done via @ref enqueue, which returns a `std::future` allowing
 * the caller to obtain the result (or re-throw exceptions) asynchronously.
 *
 * ### State encoding
 * A single 64-bit atomic `task_state` encodes two counters and a shutdown flag:
 * | Bits  | Meaning                                      |
 * |-------|----------------------------------------------|
 * | 63–32 | Pending tasks (enqueued, not yet picked up)   |
 * | 31    | Shutdown-requested flag, set by `shutdown()`  |
 * | 30–0  | Active tasks (currently executing)            |
 *
 * Workers park on `task_state.wait()` when no pending work is available and
 * are woken by `task_state.notify_one()` after each submission, or by
 * `task_state.notify_all()` once `shutdown()` sets the flag above.
 *
 * @par Fix: shutdown signalling
 * The shutdown flag used to be a separate `std::atomic<bool> stop`, with
 * `shutdown()` additionally overwriting `task_state` with placeholder values
 * purely to force any parked worker to wake up. That "destructive wakeup"
 * clobbered the real pending/active counts. Folding the flag into
 * `task_state` itself removes the need to touch the counters at all:
 * `task_state.wait(old)` is specified to return immediately if the atomic's
 * current value no longer equals `old` — so setting @ref STOP_BIT is
 * guaranteed to either be observed by a worker before it parks, or to make
 * its very next `wait()` call return immediately.
 *
 * @note `LockFreeQueue<MoveOnlyTask>` now reclaims nodes via hazard pointers
 * instead of `atomic<shared_ptr<Node>>` — see that class's doc comment. This
 * is transparent to `ThreadPool`; its interface (`enqueue`/`dequeue`) is
 * unchanged.
 *
 * ### Typical usage
 * @code
 *   ThreadPool pool(std::thread::hardware_concurrency());
 *
 *   auto f = pool.enqueue([](int x){ return x * x; }, 7);
 *   std::cout << f.get() << '\n'; // 49
 *
 *   pool.waitAllTasksCompleted();
 *   pool.shutdown();
 * @endcode
 *
 * @note The destructor calls @ref shutdown automatically, so explicit shutdown
 *       is only necessary when you need to drain the pool before destruction.
 */
class ThreadPool {
private:
    const std::size_t num_threads;

    alignas(64) std::atomic<uint64_t> task_state{0};

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);
    static constexpr uint64_t STOP_BIT    = uint64_t(1) << 31;
    static constexpr uint64_t ACTIVE_MASK = STOP_BIT - 1;

    std::vector<std::thread> workers;
    LockFreeQueue<MoveOnlyTask> task_queue;
    alignas(64) std::atomic<uint64_t> global_waiters{0};

    static uint32_t pendingFromState(uint64_t s) noexcept { return static_cast<uint32_t>(s >> 32); }
    static uint32_t activeFromState(uint64_t s) noexcept  { return static_cast<uint32_t>(s & ACTIVE_MASK); }
    static bool stopFromState(uint64_t s) noexcept { return (s & STOP_BIT) != 0; }

    void runTask(MoveOnlyTask& task);

    void workerThread();

public:
    explicit ThreadPool(std::size_t n);

    ~ThreadPool();

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto promise = std::make_shared<std::promise<return_type>>();
        std::future<return_type> result = promise->get_future();

        task_queue.enqueue(MoveOnlyTask([
            func = std::forward<F>(f),
            tup  = std::make_tuple(std::forward<Args>(args)...),
            p    = std::move(promise)]() mutable
        {
            try {
                if constexpr (std::is_void_v<return_type>) {
                    std::apply(std::move(func), std::move(tup));
                    p->set_value();
                } else {
                    p->set_value(std::apply(std::move(func), std::move(tup)));
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        }));

        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        task_state.notify_one();

        return result;
    }

    void waitAllTasksCompleted();

    void shutdown();

    bool isIdle() const {
        uint64_t s = task_state.load(std::memory_order_acquire);
        return pendingFromState(s) == 0 && activeFromState(s) == 0;
    }

    std::size_t threadCount() const { return num_threads; }
    uint64_t pendingCount() const { return pendingFromState(task_state.load(std::memory_order_acquire)); }
    uint64_t activeCount() const { return activeFromState(task_state.load(std::memory_order_acquire)); }
};


/**
 * @brief Thread-safe retrieval of the process-wide ThreadPool singleton.
 *        Defined in src/core/threadpool.cpp.
 */
ThreadPool& getStaticThreadPool();

#endif // THREADPOOL_H
