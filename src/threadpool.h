// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Project Headers
#include "./concurrency.h"

/**
 * @file thread_pool.hpp
 * @brief Lock-free thread pool with type-erased move-only tasks.
 *
 * Provides three cooperative components:
 *  - @ref MoveOnlyTask  – a small-buffer-optimised, move-only type-erased callable.
 *  - @ref LockFreeQueue – a Michael–Scott lock-free FIFO queue.
 *  - @ref ThreadPool    – a fully lock-free worker-thread pool built on the two above.
 *
 * @note Requires C++20 (`std::atomic::wait` / `notify_*`).
 */

// ─────────────────────────────────────────────────────────────────────────────
//  MoveOnlyTask
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class MoveOnlyTask
 * @brief A move-only, type-erased callable wrapper with small-buffer optimisation.
 *
 * Stores any callable (function pointer, lambda, `std::bind` expression, …) that
 * accepts zero arguments and returns `void`.  The callable is held inside an
 * inline storage buffer of @ref StorageSize bytes whenever possible; otherwise it
 * is heap-allocated via `std::unique_ptr` and the pointer itself lives in the
 * buffer.  Either way the external interface – move, call, destroy – is uniform
 * through a compile-time-generated VTable.
 *
 * Copy construction and copy assignment are explicitly deleted because the stored
 * callable may itself be non-copyable (e.g. a lambda that captures a
 * `std::unique_ptr`).
 *
 * ### Typical usage
 * @code
 *   MoveOnlyTask task = []{ std::puts("hello"); };
 *   task();   // prints "hello"
 *
 *   MoveOnlyTask moved = std::move(task);
 *   moved();  // still prints "hello"
 *   // task is now empty (operator bool() == false)
 * @endcode
 */
class MoveOnlyTask {
private:
    /// @brief Maximum number of bytes that may be stored inline (without heap allocation).
    static constexpr std::size_t StorageSize = 120;

    /**
     * @brief Per-type virtual dispatch table.
     *
     * A single instance of this struct is generated at compile time for each
     * concrete callable type @c F via the `vtable_for<F>` / `vtable_for_heap<F>`
     * static members.  All three function pointers receive the raw storage pointer
     * and must cast it to the correct type internally.
     */
    struct VTable {
        void (*call)(void*);           ///< Invoke the stored callable.
        void (*destroy)(void*) noexcept; ///< Destroy the stored callable in-place.
        void (*move)(void*, void*) noexcept; ///< Move-construct from @p src into @p dst.
    };

    /// @brief Aligned raw storage for the callable (or a `unique_ptr` to it).
    alignas(std::max_align_t) char storage[StorageSize];

    /// @brief Active dispatch table, or `nullptr` when the task is empty.
    const VTable* vtable{nullptr};

    // ── Inline-storage VTable implementations ────────────────────────────────

    /**
     * @brief Calls the callable stored at @p ptr by invoking `operator()`.
     * @tparam F Decayed callable type stored inline.
     * @param ptr Pointer to inline storage containing an object of type @c F.
     */
    template <typename F>
    static void call_impl(void* ptr) {
        (*std::launder(static_cast<F*>(ptr)))();
    }

    /**
     * @brief Destroys the callable stored at @p ptr by calling its destructor.
     * @tparam F Decayed callable type stored inline.
     * @param ptr Pointer to inline storage containing an object of type @c F.
     */
    template <typename F>
    static void destroy_impl(void* ptr) noexcept {
        std::launder(static_cast<F*>(ptr))->~F();
    }

    /**
     * @brief Move-constructs an @c F from @p src into @p dst.
     * @tparam F Decayed callable type stored inline.
     * @param dst Destination raw storage (uninitialised).
     * @param src Source raw storage containing an object of type @c F.
     */
    template <typename F>
    static void move_impl(void* dst, void* src) noexcept {
        ::new (dst) F(std::move(*std::launder(static_cast<F*>(src))));
    }

    /**
     * @brief Compile-time VTable instance for callables that fit in inline storage.
     * @tparam F Decayed callable type.
     */
    template <typename F>
    static constexpr VTable vtable_for = { &call_impl<F>, &destroy_impl<F>, &move_impl<F> };

    // ── Heap-storage VTable implementations ──────────────────────────────────

    /**
     * @brief Calls the callable stored behind a `unique_ptr<F>` at @p ptr.
     * @tparam F Decayed callable type allocated on the heap.
     * @param ptr Pointer to inline storage containing a `std::unique_ptr<F>`.
     */
    template <typename F>
    static void call_heap_impl(void* ptr) {
        auto* up = std::launder(static_cast<std::unique_ptr<F>*>(ptr));
        (**up)();
    }

    /**
     * @brief Destroys the `unique_ptr<F>` (and thus the heap callable) at @p ptr.
     * @tparam F Decayed callable type allocated on the heap.
     * @param ptr Pointer to inline storage containing a `std::unique_ptr<F>`.
     */
    template <typename F>
    static void destroy_heap_impl(void* ptr) noexcept {
        using Box = std::unique_ptr<F>;
        std::launder(static_cast<Box*>(ptr))->~Box();
    }

    /**
     * @brief Move-constructs the `unique_ptr<F>` from @p src into @p dst.
     * @tparam F Decayed callable type allocated on the heap.
     * @param dst Destination raw storage (uninitialised).
     * @param src Source raw storage containing a `std::unique_ptr<F>`.
     */
    template <typename F>
    static void move_heap_impl(void* dst, void* src) noexcept {
        using Box = std::unique_ptr<F>;
        ::new (dst) Box(std::move(*std::launder(static_cast<Box*>(src))));
    }

    /**
     * @brief Compile-time VTable instance for callables that exceed inline storage.
     * @tparam F Decayed callable type.
     */
    template <typename F>
    static constexpr VTable vtable_for_heap = { &call_heap_impl<F>, &destroy_heap_impl<F>, &move_heap_impl<F> };

public:
    // ── Constructors / destructor ─────────────────────────────────────────────

    /// @brief Constructs an empty (null) task.
    MoveOnlyTask() noexcept = default;

    /**
     * @brief Constructs a task wrapping the callable @p f.
     *
     * If `sizeof(DecayedF) <= StorageSize` and `alignof(DecayedF) <= alignof(std::max_align_t)`
     * the callable is constructed directly inside @ref storage.  Otherwise it is
     * heap-allocated and a `std::unique_ptr` owning it is placed in @ref storage.
     *
     * @tparam F    Callable type (deduced).  Must not be `MoveOnlyTask` itself.
     * @param  f    Forwarding reference to the callable to be stored.
     */
    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyTask>>>
    MoveOnlyTask(F&& f) {
        using DecayedF = std::decay_t<F>;
        if constexpr (sizeof(DecayedF) <= StorageSize && alignof(DecayedF) <= alignof(std::max_align_t)) {
            ::new (static_cast<void*>(storage)) DecayedF(std::forward<F>(f));
            vtable = &vtable_for<DecayedF>;
        } else {
            using Box = std::unique_ptr<DecayedF>;
            ::new (static_cast<void*>(storage)) Box(std::make_unique<DecayedF>(std::forward<F>(f)));
            vtable = &vtable_for_heap<DecayedF>;
        }
    }

    /// @brief Destroys the stored callable (if any) via @ref reset.
    ~MoveOnlyTask() noexcept { reset(); }

    /// @brief Copy construction is disabled; the stored callable may be non-copyable.
    MoveOnlyTask(const MoveOnlyTask&) = delete;
    /// @brief Copy assignment is disabled; the stored callable may be non-copyable.
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    /**
     * @brief Move-constructs from @p other, leaving it empty.
     * @param other Source task.  Will be empty after this call.
     */
    MoveOnlyTask(MoveOnlyTask&& other) noexcept { move_from(std::move(other)); }

    /**
     * @brief Move-assigns from @p other, leaving it empty.
     *
     * Destroys the currently stored callable (if any) before taking ownership of
     * @p other's callable.  Self-assignment is handled safely.
     *
     * @param other Source task.  Will be empty after this call.
     * @return `*this`
     */
    MoveOnlyTask& operator=(MoveOnlyTask&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    // ── Observers / mutators ─────────────────────────────────────────────────

    /**
     * @brief Invokes the stored callable.
     *
     * Does nothing if the task is empty (`operator bool() == false`).
     */
    void operator()() {
        if (vtable) {
            vtable->call(storage);
        }
    }

    /**
     * @brief Returns `true` if this task holds a callable.
     * @return `true` when non-empty, `false` when default-constructed or after a move.
     */
    explicit operator bool() const noexcept { return vtable != nullptr; }

    /**
     * @brief Destroys the stored callable and resets the task to the empty state.
     *
     * After this call `operator bool()` returns `false`.  Safe to call on an
     * already-empty task.
     */
    void reset() noexcept {
        if (vtable) {
            vtable->destroy(storage);
            vtable = nullptr;
        }
    }

private:
    /**
     * @brief Takes ownership of @p other's callable by moving it into this object's storage.
     *
     * Assumes this object's storage is uninitialised (i.e. `vtable == nullptr`).
     * After the call @p other is left empty.
     *
     * @param other Source task.
     */
    void move_from(MoveOnlyTask&& other) noexcept {
        vtable = other.vtable;
        if (vtable) {
            vtable->move(storage, other.storage);
            other.vtable = nullptr;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  LockFreeQueue
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class LockFreeQueue
 * @brief Thread-safe, non-blocking Michael–Scott lock-free FIFO queue.
 *
 * Implements the classic Michael & Scott (1996) two-pointer CAS-based queue.
 * Both @ref enqueue and @ref dequeue are linearisable and wait-free in the
 * absence of contention; under contention they are lock-free (at least one
 * thread makes progress per finite number of steps).
 *
 * The queue uses a sentinel (dummy) head node so that head and tail never
 * alias the same node while the queue is non-empty, simplifying the CAS logic.
 *
 * @tparam T Element type.  Must be movable.
 *
 * @note `std::shared_ptr` is used for node ownership to avoid the ABA problem
 *       that would occur with raw pointers and manual memory management.
 *
 * ### Typical usage
 * @code
 *   LockFreeQueue<int> q;
 *   q.enqueue(42);
 *
 *   int val;
 *   if (q.dequeue(val)) {
 *       // val == 42
 *   }
 * @endcode
 */
template <typename T>
class LockFreeQueue {
private:
    /**
     * @brief Internal singly-linked list node.
     *
     * @c data holds the element value (only meaningful in non-sentinel nodes).
     * @c next is an atomic `shared_ptr` to the successor node.
     */
    struct Node {
        T data;                                  ///< Stored element (invalid in sentinel).
        std::atomic<std::shared_ptr<Node>> next; ///< Atomic link to the next node.

        /// @brief Constructs a sentinel (empty) node.
        Node() : data(), next(nullptr) {}
        /// @brief Constructs a data node with @p val.
        Node(T&& val) : data(std::move(val)), next(nullptr) {}
    };

    /// @brief Points to the sentinel node; elements are dequeued from @c head->next.
    alignas(64) std::atomic<std::shared_ptr<Node>> head;
    /// @brief Points to the last enqueued node (may lag one step behind the true tail).
    alignas(64) std::atomic<std::shared_ptr<Node>> tail;

public:
    /**
     * @brief Constructs an empty queue with a single sentinel node.
     *
     * Both @c head and @c tail are initialised to point at the same dummy node.
     */
    LockFreeQueue() {
        auto dummy = std::make_shared<Node>();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    /**
     * @brief Appends @p value to the back of the queue.
     *
     * Allocates a new node, then uses a CAS loop to link it after the current
     * tail.  If the tail pointer has fallen behind (another thread enqueued but
     * did not yet swing the tail), this thread helps advance it first.
     *
     * @param value The value to enqueue.  Moved into the new node.
     */
    void enqueue(T value) {
        auto new_node = std::make_shared<Node>(std::move(value));
        while (true) {
            auto last = tail.load(std::memory_order_acquire);
            auto next = last->next.load(std::memory_order_acquire);

            if (last == tail.load(std::memory_order_relaxed)) {
                if (next == nullptr) {
                    // Tail truly points to the last node; try to link new_node.
                    if (last->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release, std::memory_order_relaxed)) {
                        // Link succeeded; try to swing tail (failure is benign).
                        tail.compare_exchange_weak(last, new_node, std::memory_order_release);
                        return;
                    }
                } else {
                    // Tail is lagging; help advance it.
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                }
            }
            pause_processor();
        }
    }

    /**
     * @brief Removes and returns the element at the front of the queue.
     *
     * Uses a CAS on @c head to atomically claim ownership of the first real
     * node.  The thread that wins the CAS is the sole owner of that node's
     * @c data and moves it into @p result.
     *
     * @param[out] result  Receives the dequeued value on success.
     * @return `true` if an element was dequeued, `false` if the queue was empty.
     */
    bool dequeue(T& result) {
        while (true) {
            auto first = head.load(std::memory_order_acquire);
            auto last  = tail.load(std::memory_order_acquire);
            auto next  = first->next.load(std::memory_order_acquire);

            if (first == head.load(std::memory_order_relaxed)) {
                if (first == last) {
                    if (next == nullptr) return false; // Truly empty.
                    // Tail is lagging; help advance it.
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                } else {
                    // Attempt to swing head to the next node.
                    if (head.compare_exchange_weak(first, next, std::memory_order_acq_rel)) {
                        // Sole owner of next->data; extract it.
                        result = std::move(next->data);
                        return true;
                    }
                }
            }
            pause_processor();
        }
    }

private:
    /**
     * @brief Emits a CPU spin-loop hint or yields the thread.
     *
     * On x86-64 issues the `PAUSE` instruction to reduce pipeline stalls in
     * spin loops.  On other architectures falls back to `std::this_thread::yield()`.
     */
    static inline void pause_processor() noexcept {
        #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
        #else
            std::this_thread::yield();
        #endif
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ThreadPool
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class ThreadPool
 * @brief Fully lock-free thread pool backed by C++20 atomic wait/notify.
 *
 * Manages a fixed set of worker threads that drain tasks from a @ref LockFreeQueue.
 * Task submission is done via @ref enqueue, which returns a `std::future` allowing
 * the caller to obtain the result (or re-throw exceptions) asynchronously.
 *
 * ### State encoding
 * A single 64-bit atomic `task_state` encodes two independent 32-bit counters:
 * | Bits  | Meaning                            |
 * |-------|------------------------------------|
 * | 63–32 | Pending tasks (enqueued, not yet picked up) |
 * | 31–0  | Active tasks (currently executing) |
 *
 * Workers park on `task_state.wait()` when no pending work is available and
 * are woken by `task_state.notify_one()` after each submission.
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
    /// @brief Number of worker threads in this pool.
    const size_t num_threads;

    /**
     * @brief Combined pending/active task counter.
     *
     * Upper 32 bits = number of tasks that have been enqueued but not yet
     * picked up by a worker.  Lower 32 bits = number of tasks currently
     * executing.  Both counters are manipulated with a single atomic add,
     * which keeps them consistent without a separate lock.
     */
    alignas(64) std::atomic<uint64_t> task_state{0};

    /// @brief Addend that increments the pending count by one.
    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    /// @brief Addend that increments the active count by one.
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    /// @brief Worker thread handles.
    std::vector<std::thread> workers;

    /// @brief FIFO queue of pending tasks.
    LockFreeQueue<MoveOnlyTask> task_queue;

    /// @brief Set to `true` by @ref shutdown to signal workers to exit.
    alignas(64) std::atomic<bool> stop{false};

    /// @brief Reserved for future use (e.g. tracking waiters on @ref waitAllTasksCompleted).
    alignas(64) std::atomic<uint64_t> global_waiters{0};

    // ── State helper accessors ────────────────────────────────────────────────

    /**
     * @brief Extracts the pending task count from a packed state value.
     * @param s Packed value loaded from @ref task_state.
     * @return Number of tasks that are enqueued but not yet executing.
     */
    static uint32_t pendingFromState(uint64_t s) noexcept { return static_cast<uint32_t>(s >> 32); }

    /**
     * @brief Extracts the active task count from a packed state value.
     * @param s Packed value loaded from @ref task_state.
     * @return Number of tasks currently executing.
     */
    static uint32_t activeFromState(uint64_t s) noexcept  { return static_cast<uint32_t>(s & 0xFFFFFFFFULL); }

    // ── Internal worker helpers ───────────────────────────────────────────────

    /**
     * @brief Executes a single task and decrements the active counter.
     *
     * Exceptions thrown by the task are silently swallowed (callers receive
     * exceptions via the `std::future` returned from @ref enqueue).  After the
     * task completes the active counter is decremented; if that brings both
     * counters to zero any threads blocked in @ref waitAllTasksCompleted are
     * notified.
     *
     * @param task The task to execute.  Must be non-empty.
     */
    void runTask(MoveOnlyTask& task) {
        if (task) {
            try { task(); } catch (...) {}
        }

        // Atomically drop active-execution status.
        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);

        // Notify waitAllTasksCompleted() if this was the last in-flight task.
        if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
            task_state.notify_all();
        }
    }

    /**
     * @brief Entry point for each worker thread.
     *
     * Coordinates execution using a speculative, lock-free state machine:
     * 0. Evaluates global shutdown state first, exiting instantly if stop is signaled.
     * 1. Synchronizes via state-based parking; blocks on `task_state.wait()`
     * at 0% CPU whenever the global pending task count is zero.
     * 2. Speculatively claims a task by atomically transferring one credit
     * from 'pending' to 'active' in the 64-bit `task_state` bitfield.
     * 3. Attempts to physically extract a payload from the `LockFreeQueue`.
     * 4. On success: Executes the task safely wrapped in a try/catch block.
     * 5. On failure (queue race collision): Executes a fail-safe state rollback
     * and strategically yields the thread execution timeslice to prevent
     * userspace livelocks, allowing the atomic state to settle.
     */
    void workerThread() {
        while (true) {
            // 1. ABSOLUTE EXIT GATE: If stop is set, exit immediately.
            if (stop.load(std::memory_order_acquire)) {
                return;
            }

            uint64_t current_state = task_state.load(std::memory_order_acquire);

            // 2. PASSIVE PARK GATE
            while (pendingFromState(current_state) == 0) {
                // Check right before sleeping
                if (stop.load(std::memory_order_acquire)) return;

                task_state.wait(current_state, std::memory_order_relaxed);
                current_state = task_state.load(std::memory_order_acquire);

                // Check right after waking up
                if (stop.load(std::memory_order_acquire)) return;
            }

            // 3. SPECULATIVE CREDIT CLAIM
            uint64_t expected = current_state;
            uint64_t desired = current_state - PENDING_ONE + ACTIVE_ONE;

            if (!task_state.compare_exchange_weak(expected, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                #else
                    std::this_thread::yield();
                #endif
                continue;
            }

            // 4. PHYSICAL QUEUE EXTRACTION
            MoveOnlyTask task;
            if (task_queue.dequeue(task)) {
                runTask(task);
            } else {
                // Rollback State: Fix counter offset
                uint64_t prev = task_state.fetch_add(PENDING_ONE - ACTIVE_ONE, std::memory_order_acq_rel);
                if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
                    task_state.notify_all();
                }

                // 5. LIVELOCK & SHUTDOWN SAFETY PROTECTION
                // If shutdown happens during rollback, break out immediately
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }

                std::this_thread::yield();
            }
        }
    }

public:
    // ── Construction / destruction ────────────────────────────────────────────

    /**
     * @brief Constructs a thread pool with @p n worker threads.
     *
     * Launches @p n threads immediately; they begin polling for work as soon as
     * @ref enqueue is called.
     *
     * @param n Number of worker threads.  Must be > 0.
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
     * @brief Destroys the pool, calling @ref shutdown if it has not been called yet.
     *
     * Blocks until all worker threads have exited.
     */
    ~ThreadPool() {
        shutdown();
    }

    // ── Public interface ──────────────────────────────────────────────────────

    /**
     * @brief Submits a callable and its arguments for asynchronous execution.
     *
     * The callable and arguments are captured by value (or move) into a
     * @ref MoveOnlyTask.  The task is enqueued and one waiting worker is
     * notified.  The returned `std::future` provides access to the return
     * value or any exception thrown by @p f.
     *
     * @tparam F    Callable type (deduced).
     * @tparam Args Argument types (deduced).
     * @param  f    Callable to invoke.
     * @param  args Arguments forwarded to @p f.
     * @return A `std::future<R>` where `R = std::invoke_result_t<F, Args...>`.
     *
     * @note Calling this after @ref shutdown has been invoked results in
     *       undefined behaviour (the task is enqueued but no worker will
     *       pick it up).
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto promise = std::make_shared<std::promise<return_type>>();
        std::future<return_type> result = promise->get_future();

        // Enqueue the wrapped task before incrementing the pending counter so
        // that workers never observe a non-zero counter with an empty queue.
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

        // Now safe to signal availability to workers.
        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        task_state.notify_one();

        return result;
    }

    /**
     * @brief Blocks the calling thread until all pending and active tasks finish.
     *
     * Uses `std::atomic::wait` (C++20 futex) to avoid a busy spin.  Returns
     * immediately if the pool is already idle.
     *
     * @note This does not prevent new tasks from being submitted concurrently,
     *       so the function may block for longer than expected in that case.
     */
    void waitAllTasksCompleted() {
        uint64_t current_state = task_state.load(std::memory_order_acquire);
        while (current_state != 0) {
            task_state.wait(current_state, std::memory_order_relaxed);
            current_state = task_state.load(std::memory_order_acquire);
        }
    }

    /**
         * @brief Signals all workers to stop, joins them, and drains the task queue.
         *
         * Idempotent: calling @ref shutdown more than once is safe.  Any tasks
         * that were enqueued but not yet started are dequeued and discarded (their
         * associated `std::future` will never become ready).
         *
         * @warning Do not call @ref enqueue after @ref shutdown.
         */
        void shutdown() {
            // 1. Signal the stop flag
            stop.store(true, std::memory_order_release);

            // 2. Clear out the state entirely and shake any parked threads awake from the kernel.
            // Changing the value ensures task_state.wait() comparisons instantly fail.
            task_state.store(0, std::memory_order_release);
            task_state.notify_all();

            // 3. Join all worker threads to guarantee they have completely stopped executing
            for (std::thread& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            // CRITICAL DRAINING STEP: Discard any remaining tasks left behind in the queue.
            // This ensures unexecuted tasks are destroyed, releasing their resources
            // and breaking any associated std::promise/std::future chains as promised.
            MoveOnlyTask abandoned_task;
            while (task_queue.dequeue(abandoned_task)) {
                // Intentionally do nothing; letting 'abandoned_task' go out of scope
                // destroys the underlying lambda/functor and fails its promise.
            }
        }

    // ── Observers ─────────────────────────────────────────────────────────────

    /**
     * @brief Returns `true` if there are no pending or active tasks.
     * @return `true` when both the pending and active counters are zero.
     */
    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }

    /**
     * @brief Returns the number of worker threads managed by this pool.
     * @return The value passed to the constructor.
     */
    size_t threadCount() const { return num_threads; }

    /**
     * @brief Returns the number of tasks that have been enqueued but not yet started.
     * @return Pending task count extracted from @ref task_state.
     */
    uint64_t pendingCount() const { return pendingFromState(task_state.load(std::memory_order_acquire)); }

    /**
     * @brief Returns the number of tasks currently being executed by workers.
     * @return Active task count extracted from @ref task_state.
     */
    uint64_t activeCount() const { return activeFromState(task_state.load(std::memory_order_acquire)); }
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
