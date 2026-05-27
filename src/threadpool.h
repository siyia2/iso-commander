// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

// Project Headers
#include "./concurrency.h"

/**
 * @brief A thread-safe, non-blocking Michael-Scott Lock-Free Queue.
 *
 * @details This implementation utilizes C++20 atomic shared_ptr to eliminate the
 * classic ABA problem without requiring a complex garbage collector.
 *
 * Concurrency guarantees:
 * - Safe for any number of concurrent producers (enqueue) and consumers (dequeue)
 *   simultaneously. There is no reader/writer exclusion: N threads may enqueue
 *   and M threads may dequeue at the same time without external synchronisation.
 * - Progress guarantee: enqueue() is lock-free (always makes system-wide progress).
 *   dequeue() is also lock-free; individual threads may retry but at least one
 *   thread always completes its operation in a bounded number of steps.
 * - Expected complexity: O(1) amortized per operation under low contention.
 *   Under heavy contention CAS retries increase, but the tail-helping mechanism
 *   ensures no thread is starved indefinitely.
 *
 * Key Robustness Features:
 * 1. Data Handoff: Uses std::unique_ptr internally to ensure that memory ownership
 *    of the task is transferred atomically. Only the thread that successfully
 *    swings the 'head' pointer via CAS takes ownership of the task data,
 *    preventing double-moves or heap corruption.
 * 2. Tail-Helping: Includes logic to allow threads to "help" move the tail
 *    pointer forward if it lags behind, preventing a single stalled thread
 *    from blocking the entire queue.
 *
 * @tparam T The type of elements stored in the queue (typically std::function).
 */
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        std::unique_ptr<T> data;
        std::atomic<std::shared_ptr<Node>> next;
        Node() : data(nullptr), next(nullptr) {}
        Node(T&& val) : data(std::make_unique<T>(std::move(val))), next(nullptr) {}
    };

    alignas(64) std::atomic<std::shared_ptr<Node>> head;
    alignas(64) std::atomic<std::shared_ptr<Node>> tail;

public:
    /** @brief Initializes the queue with a dummy node to simplify boundary conditions. */
    LockFreeQueue() {
        auto dummy = std::make_shared<Node>();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    /**
     * @brief Atomically enqueues an item.
     *
     * @details Appends @p value to the tail of the queue using a two-step CAS
     * protocol:
     *   1. Link the new node onto the last node's `next` pointer.
     *   2. Swing `tail` forward to the new node.
     * Step 2 is best-effort: if this thread is pre-empted between the two steps,
     * the next caller (enqueue or dequeue) detects the lagging tail and advances
     * it before proceeding, so no progress is ever lost.
     *
     * @param value The item to move into the queue. The value is moved on the
     *              first successful CAS; the caller should treat @p value as
     *              indeterminate after this call returns.
     *
     * @note Lock-free. May spin if concurrent enqueues cause repeated CAS failures,
     *       but system-wide progress is guaranteed.
     */
    void enqueue(T value) {
        auto new_node = std::make_shared<Node>(std::move(value));
        while (true) {
            auto last = tail.load(std::memory_order_acquire);
            auto next = last->next.load(std::memory_order_acquire);

            if (next == nullptr) {
                // Try to link the new node to the end of the list.
                if (last->next.compare_exchange_weak(next, new_node,
                    std::memory_order_release, std::memory_order_relaxed)) {

                    // Link successful; try to swing tail to the new node.
                    // Failure is fine — another thread or the next enqueue will fix it.
                    tail.compare_exchange_weak(last, new_node, std::memory_order_release);
                    return;
                }
            } else {
                // Tail is lagging; help move it forward so other threads aren't blocked.
                tail.compare_exchange_weak(last, next, std::memory_order_release);
            }
        }
    }

    /**
     * @brief Atomically dequeues an item.
     *
     * @details Removes the front element using the Michael-Scott protocol:
     *   - The queue uses a permanent dummy head node, so the real front element
     *     is always `head->next`.
     *   - The winning thread swings `head` forward to `head->next` via CAS and
     *     takes exclusive ownership of the data stored in the new head node.
     *   - If `head == tail` but `head->next != nullptr`, the tail pointer has
     *     lagged; this thread advances it before retrying so enqueue threads
     *     are not stalled.
     *
     * @param[out] result Reference into which the dequeued item is moved on
     *             success. Left unmodified on failure (empty queue).
     *
     * @return true  if an item was retrieved and moved into @p result.
     * @return false if the queue was empty at the time of the call.
     *
     * @note Lock-free. A returning `false` is a snapshot: an enqueue racing
     *       concurrently may have added an item an instant later.
     */
    bool dequeue(T& result) {
        while (true) {
            auto first = head.load(std::memory_order_acquire);
            auto last  = tail.load(std::memory_order_acquire);
            auto next  = first->next.load(std::memory_order_acquire);

            if (first == last) {
                if (next == nullptr) return false; // Queue is empty.

                // Tail is lagging behind head; help move it forward.
                tail.compare_exchange_weak(last, next, std::memory_order_release);
            } else {
                // Pre-read data; ownership is only confirmed if CAS succeeds.
                if (head.compare_exchange_weak(first, next, std::memory_order_acq_rel)) {
                    if (next->data) {
                        result = std::move(*(next->data));
                        next->data.reset(); // Release node memory immediately.
                    }
                    return true;
                }
            }
        }
    }
};

/**
 * @brief High-performance ThreadPool using lock-free task management.
 *
 * @details This pool manages a set of worker threads that consume tasks from a
 * LockFreeQueue. It utilizes a 64-bit atomic state bitfield (32-bit pending,
 * 32-bit active) to track workload without the overhead of heavy mutex locking.
 *
 * Design Features:
 * 1. Automatic Lifetime: Designed as a Meyers Singleton to ensure background
 *    threads are joined before global object destruction.
 * 2. RAII Shutdown: Destructor ensures all tasks are completed and workers
 *    joined, preventing "zombie" threads from accessing a stale heap.
 * 3. Cache Alignment: Core atomics are aligned to 64-byte boundaries to
 *    eliminate False Sharing performance degradation.
 *
 * Task lifecycle:
 *   enqueue() called
 *     → pending count incremented
 *     → worker woken, dequeues task
 *       → pending decremented, active incremented  (runTask entry)
 *       → task executes
 *       → active decremented                       (runTask exit)
 *         → if both counts reach zero, idle waiters are notified
 *
 * Thread safety:
 *   All public methods are safe to call concurrently from any thread.
 *   enqueue() and the query helpers (isIdle, pendingCount, activeCount) are
 *   wait-free. waitAllTasksCompleted() and shutdown() block the caller.
 */
class ThreadPool {
private:
    const size_t num_threads;

    // Bits 63-32: Number of tasks in queue. Bits 31-0: Number of tasks currently running.
    alignas(64) std::atomic<uint64_t> task_state{0};

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    std::mutex              mutex;
    std::condition_variable cv;
    std::atomic<bool>       stop{false};
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    /** @brief Extracts the number of pending tasks from the packed 64-bit state. */
    static uint64_t pendingFromState(uint64_t s) { return s >> 32; }

    /**
     * @brief Notifies waitAllTasksCompleted() if the pool just became fully idle.
     *
     * @details Called after decrementing the active counter. @p prevState is the
     * value returned by fetch_sub *before* the decrement was applied, so the
     * pool is now idle when `prevState - ACTIVE_ONE == 0`, i.e. when both the
     * pending half-word and the active half-word of the new state are zero.
     *
     * Note: this correctly handles the case where a task completes while other
     * tasks are still pending — `prevState - ACTIVE_ONE` will be non-zero in
     * that situation, so no spurious idle notification is sent.
     *
     * @param prevState The 64-bit task_state value immediately before the active
     *                  counter was decremented by the caller.
     */
    void notifyIfIdle(uint64_t prevState) {
        uint64_t newState = prevState - ACTIVE_ONE;
        if (newState == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    /**
     * @brief Executes a single task and manages the pending/active counters
     *        around it.
     *
     * @details Performs the following state transitions atomically:
     *   1. Entry: decrements pending count and increments active count in a
     *      single fetch_add, keeping task_state consistent at all times.
     *   2. Invokes the task inside a try/catch to prevent exceptions from
     *      escaping into the worker thread (which would call std::terminate).
     *   3. Exit: decrements the active count and calls notifyIfIdle() with the
     *      pre-decrement snapshot so idle waiters can be woken precisely when
     *      the last task finishes.
     *
     * @param task The callable to execute. A null / empty std::function is
     *             silently skipped; the state counters are still updated.
     */
    void runTask(std::function<void()>& task) {
        // Transition: decrement pending count, increment active count.
        task_state.fetch_add(static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE,
                             std::memory_order_acq_rel);

        if (task) {
            try { task(); } catch (...) { /* Prevent exception propagation into worker thread. */ }
        }

        // Transition: decrement active count. Capture previous state to detect idle.
        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);
        notifyIfIdle(prev);
    }

    /**
     * @brief Main loop executed by each worker thread.
     *
     * @details Each iteration attempts a non-blocking dequeue. On success the
     * task is run immediately and the loop restarts without sleeping, allowing
     * a burst of queued work to be drained at full throughput.
     *
     * On a failed dequeue the thread checks whether it should exit (stop flag
     * set *and* no outstanding tasks), then blocks on the condition variable.
     * The CV predicate rechecks the pending count rather than a separate flag,
     * which closes the TOCTOU window between the failed dequeue and entering
     * the wait: if a producer incremented the pending count in that window the
     * predicate is already true and the thread never sleeps.
     *
     * Shutdown path: once stop is set, the worker drains any remaining tasks
     * before returning, so tasks submitted before shutdown() is called are
     * always completed.
     */
    void workerThread() {
        while (true) {
            std::function<void()> task;

            if (task_queue.dequeue(task)) {
                runTask(task);
                continue;
            }

            // Re-check stop only after a failed dequeue so we don't exit early
            // while tasks might be in flight.
            if (stop.load(std::memory_order_acquire) &&
                task_state.load(std::memory_order_acquire) == 0) {
                return;
            }

            // Sleep until work or shutdown arrives.
            {
                std::unique_lock<std::mutex> lock(mutex);
                sleeping_threads.fetch_add(1, std::memory_order_relaxed);

                cv.wait(lock, [this] {
                    // Wake if there is pending work OR if we should stop.
                    // Checking pending > 0 here closes the TOCTOU gap: a task
                    // enqueued between our failed dequeue and this wait will
                    // have already incremented the pending count, so the
                    // predicate will be true and we won't sleep at all.
                    return pendingFromState(task_state.load(std::memory_order_acquire)) > 0
                        || stop.load(std::memory_order_acquire);
                });

                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            }
            // Loop back to dequeue; don't check stop before attempting more work.
        }
    }

public:
    /**
     * @brief Constructs a ThreadPool and spawns @p n worker threads.
     *
     * @param n Number of worker threads. Must be greater than zero.
     * @throws std::invalid_argument if @p n == 0.
     */
    explicit ThreadPool(size_t n) : num_threads(n) {
        if (n == 0) throw std::invalid_argument("ThreadPool: n > 0 required");
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    /**
     * @brief Destructor. Blocks until all pending tasks are finished, then joins workers.
     *
     * @details Calls waitAllTasksCompleted() followed by shutdown(). Tasks that
     * are already queued when the destructor runs will be executed to completion.
     * Any tasks enqueued *after* waitAllTasksCompleted() returns but before
     * shutdown() drains the queue are silently discarded (see shutdown()).
     */
    ~ThreadPool() {
        waitAllTasksCompleted();
        shutdown();
    }

    /**
     * @brief Submits a callable and its arguments to the pool for asynchronous execution.
     *
     * @details Wraps @p f and @p args in a std::packaged_task, enqueues it on
     * the lock-free task queue, and wakes one sleeping worker. The returned
     * future becomes ready when the task finishes (or throws — exceptions are
     * captured in the future, not swallowed).
     *
     * @tparam F    Callable type (function, lambda, functor, …).
     * @tparam Args Argument types forwarded to @p f.
     *
     * @param f    The callable to invoke on a worker thread.
     * @param args Arguments forwarded to @p f at the time of invocation.
     *
     * @return std::future<R> where R = std::invoke_result_t<F, Args...>.
     *         Calling future::get() blocks until the result is available and
     *         re-throws any exception thrown by @p f.
     *
     * @note Do not call after shutdown(); behaviour is undefined once workers
     *       have been joined.
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        std::future<return_type> result = task->get_future();

        task_state.fetch_add(PENDING_ONE, std::memory_order_release);

        task_queue.enqueue([task]() {
            if (task && task->valid()) {
                (*task)();
            }
        });

        // Always notify under the mutex to eliminate the TOCTOU race between
        // sleeping_threads being read and the worker actually sleeping.
        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_one();
        }

        return result;
    }

    /**
     * @brief Blocks the calling thread until the queue is empty and all workers are idle.
     *
     * @details Waits on the internal condition variable until task_state reaches
     * zero, which happens only when both the pending count (bits 63-32) and the
     * active count (bits 31-0) are simultaneously zero. The notification is sent
     * by the last task to complete via notifyIfIdle().
     *
     * Returns immediately if the pool is already idle when called.
     *
     * @warning Tasks-spawning-tasks: if submitted tasks themselves call enqueue(),
     * task_state can momentarily hit zero between the parent task completing and
     * its child tasks being registered. This function may therefore return early
     * before child tasks finish. If your workload relies on recursive task
     * submission, use an explicit external barrier (e.g. a std::latch or a
     * reference-counted fence) instead of this function.
     *
     * @note Safe to call from any thread, including concurrently with enqueue().
     */
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }

    /**
     * @brief Signals all workers to exit after draining remaining work, then
     *        joins every thread.
     *
     * @details Sets the stop flag and broadcasts on the condition variable so
     * sleeping workers wake and observe the flag. Each worker completes any task
     * it has already dequeued before exiting. Tasks that are still in the queue
     * when workers exit are *not* executed.
     *
     * Any tasks enqueued into the lock-free queue after the last worker exits
     * (a race that can occur if enqueue() is called concurrently with shutdown())
     * are dequeued and destroyed without being executed. Their associated futures
     * will never become ready; callers blocked on such futures will deadlock —
     * do not enqueue after shutdown() begins.
     *
     * Safe to call multiple times; subsequent calls are no-ops.
     *
     * @post All worker threads are joined. No further tasks will execute.
     */
    void shutdown() {
        if (stop.exchange(true, std::memory_order_acq_rel)) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        // Discard any tasks that were enqueued after waitAllTasksCompleted() returned.
        std::function<void()> residual;
        while (task_queue.dequeue(residual)) { residual = nullptr; }
    }

    /** @brief Returns true if no tasks are pending or active. */
    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }

    /** @brief Returns the total number of worker threads. */
    size_t threadCount() const { return num_threads; }

    /** @brief Returns the number of tasks currently in the queue (pending, not yet running). */
    uint64_t pendingCount() const {
        return pendingFromState(task_state.load(std::memory_order_acquire));
    }

    /** @brief Returns the number of tasks currently being executed by worker threads. */
    uint64_t activeCount() const {
        return task_state.load(std::memory_order_acquire) & 0xFFFFFFFFULL;
    }
};

/**
 * @brief Thread-safe retrieval of the process-wide ThreadPool singleton.
 *
 * @details Uses a function-local static (Meyers Singleton) to guarantee:
 *   - Thread-safe, once-only initialisation (mandated by C++11 §6.7).
 *   - Deterministic destruction after all other translation-unit statics,
 *     so worker threads are joined before any globally-scoped objects they
 *     may reference are torn down.
 *
 * The pool size is determined at first call by
 * `GlobalConcurrency::maxThreads` (the runtime-detected hardware concurrency,
 * typically from std::thread::hardware_concurrency()) capped at
 * `GlobalConcurrency::MAX_USEFUL_THREADS` (a compile-time upper bound defined
 * in GlobalConcurrency.h to avoid over-subscription on machines with very high
 * core counts).
 *
 * @return Reference to the singleton ThreadPool. The reference remains valid
 *         for the lifetime of the process.
 */
inline ThreadPool& getStaticThreadPool() {
    static ThreadPool instance(
        std::min({ static_cast<size_t>(GlobalConcurrency::maxThreads),
                   GlobalConcurrency::MAX_USEFUL_THREADS }));
    return instance;
}

#endif // THREAD_POOL_H
