// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
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
 *
 * @par Revision notes
 * Two correctness fixes have been applied since the original version:
 *  1. @ref LockFreeQueue now has an explicit, iterative destructor. The
 *     implicit one recursively tears down the `shared_ptr` node chain
 *     (destroying `head` destroys its `next`, which destroys *its* `next`,
 *     …) and can overflow the stack for a queue with many undrained
 *     elements. See the destructor's doc comment below.
 *  2. @ref ThreadPool::shutdown no longer signals workers by repeatedly
 *     overwriting the live `task_state` counter with placeholder values
 *     (0, 1, 2, 3). That was intended to force-wake any parked worker, but
 *     it clobbered the real pending/active counts — reproducibly leaving
 *     `task_state` corrupted (e.g. `isIdle()` reporting `false` forever,
 *     or the packed fields underflowing into garbage) even when shutting
 *     down an already-idle pool. The shutdown signal is now a dedicated bit
 *     folded into `task_state` itself (see @ref STOP_BIT), which lets
 *     `task_state.wait()`'s own "value already changed" guarantee do the
 *     wakeup safely, with no separate flag to race against and nothing to
 *     corrupt. `shutdown()` also now reconciles the pending counter for any
 *     tasks that were queued but never claimed, so `pendingCount()` /
 *     `isIdle()` are accurate after it returns in every case.
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
     * @brief Destroys the queue, unlinking nodes iteratively.
     *
     * @par Fix
     * The implicit (compiler-generated) destructor would destroy @c head,
     * whose destructor destroys its `next`, whose destructor destroys
     * *its* `next`, and so on — an unbounded recursion, one stack frame per
     * node, for however many elements remain undrained. For a queue holding
     * a large backlog this overflows the stack (confirmed: ~2,000,000
     * undrained `int` nodes reliably segfaults with the implicit destructor
     * on an 8 MB stack).
     *
     * This destructor instead walks the chain with a single loop variable
     * and severs each node's `next` link *before* moving past it, so no
     * node's destructor ever has a live successor to cascade into — the
     * whole teardown is O(n) time with O(1) stack depth, regardless of
     * queue length.
     *
     * @note Not thread-safe — same precondition as any container's
     *       destructor: no concurrent @ref enqueue / @ref dequeue may be in
     *       flight while this runs. `memory_order_relaxed` is sufficient
     *       here for exactly that reason — there is no concurrent access to
     *       synchronize against during destruction.
     */
    ~LockFreeQueue() {
        auto node = head.load(std::memory_order_relaxed);
        while (node) {
            auto next = node->next.load(std::memory_order_relaxed);
            node->next.store(nullptr, std::memory_order_relaxed);
            node = std::move(next);
        }
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
 * (0, 1, 2, 3) purely to force any parked worker to wake up. That "destructive
 * wakeup" clobbered the real pending/active counts — reproducibly, even when
 * shutting down an already-idle pool (`isIdle()` would then report `false`
 * forever), and it could underflow into outright garbage if tasks were still
 * actively executing at the time. Folding the flag into `task_state` itself
 * removes the need to touch the counters at all: `task_state.wait(old)` is
 * specified to return immediately, without blocking, if the atomic's current
 * value no longer equals `old` — so setting @ref STOP_BIT is guaranteed to
 * either be observed by a worker before it parks, or to make its very next
 * `wait()` call return immediately. There is no window left in which a worker
 * can fall asleep and never notice, and nothing about the real counts is ever
 * overwritten.
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
     * @brief Combined pending/active/shutdown state.
     *
     * Upper 32 bits (63–32) = number of tasks that have been enqueued but not
     * yet picked up by a worker. Bit 31 = shutdown-requested flag (see
     * @ref STOP_BIT). Remaining low bits (30–0) = number of tasks currently
     * executing. All three are manipulated through this single atomic, which
     * keeps them consistent without a separate lock and lets a shutdown
     * request piggyback on the same wait/notify mechanism as normal task
     * dispatch.
     */
    alignas(64) std::atomic<uint64_t> task_state{0};

    /// @brief Addend that increments the pending count by one.
    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    /// @brief Addend that increments the active count by one.
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);
    /**
     * @brief Bit 31 of @ref task_state: set once by @ref shutdown to request
     * that all workers exit.
     *
     * Folded into @ref task_state (rather than kept as a separate atomic)
     * specifically so that a worker parked in `task_state.wait()` can never
     * miss it — see the class-level "Fix: shutdown signalling" note above.
     * Chosen as the top bit of the low 32-bit half (rather than, say, bit 63)
     * so it can never interact with the pending-count arithmetic in
     * @ref workerThread or @ref shutdown, which only ever adds/subtracts
     * whole multiples of @ref PENDING_ONE (bit 32 and above).
     */
    static constexpr uint64_t STOP_BIT    = uint64_t(1) << 31;
    /// @brief Mask isolating the active-count bits (30–0), excluding @ref STOP_BIT.
    static constexpr uint64_t ACTIVE_MASK = STOP_BIT - 1;

    /// @brief Worker thread handles.
    std::vector<std::thread> workers;

    /// @brief FIFO queue of pending tasks.
    LockFreeQueue<MoveOnlyTask> task_queue;

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
     *
     * Masks with @ref ACTIVE_MASK to exclude @ref STOP_BIT, so the shutdown
     * flag can never be misread as part of the active count.
     *
     * @param s Packed value loaded from @ref task_state.
     * @return Number of tasks currently executing.
     */
    static uint32_t activeFromState(uint64_t s) noexcept  { return static_cast<uint32_t>(s & ACTIVE_MASK); }

    /**
     * @brief Extracts the shutdown-requested flag from a packed state value.
     * @param s Packed value loaded from @ref task_state.
     * @return `true` once @ref shutdown has set @ref STOP_BIT.
     */
    static bool stopFromState(uint64_t s) noexcept { return (s & STOP_BIT) != 0; }

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
     * 0. Evaluates the shutdown flag first, exiting instantly if it is set.
     * 1. Synchronizes via state-based parking; blocks on `task_state.wait()`
     * at 0% CPU whenever the global pending task count is zero. Because the
     * shutdown flag lives inside the same atomic being waited on, a
     * `shutdown()` call is guaranteed to either be seen before parking or to
     * make the `wait()` call return immediately — there is no separate flag
     * to race against.
     * 2. Speculatively claims a task by atomically transferring one credit
     * from 'pending' to 'active' in the 64-bit `task_state` bitfield.
     * 3. Attempts to physically extract a payload from the `LockFreeQueue`.
     * 4. On success: Executes the task safely wrapped in a try/catch block.
     * 5. On failure (queue race collision): Executes a fail-safe state rollback
     * and strategically yields the thread execution timeslice to prevent
     * userspace livelocks, allowing the atomic state to settle. The shutdown
     * flag is re-checked at the top of the next iteration regardless, so no
     * separate check is needed here.
     */
    void workerThread() {
        while (true) {
            uint64_t current_state = task_state.load(std::memory_order_acquire);

            // 0. ABSOLUTE EXIT GATE: If shutdown has been requested, exit immediately.
            if (stopFromState(current_state)) {
                return;
            }

            // 1. PASSIVE PARK GATE
            while (pendingFromState(current_state) == 0) {
                task_state.wait(current_state, std::memory_order_relaxed);
                current_state = task_state.load(std::memory_order_acquire);

                // Check right after waking up.
                if (stopFromState(current_state)) return;
            }

            // 2. SPECULATIVE CREDIT CLAIM
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

            // 3. PHYSICAL QUEUE EXTRACTION
            MoveOnlyTask task;
            if (task_queue.dequeue(task)) {
                runTask(task); // 4. Execute
            } else {
                // 5. Rollback State: Fix counter offset
                uint64_t prev = task_state.fetch_add(PENDING_ONE - ACTIVE_ONE, std::memory_order_acq_rel);
                if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
                    task_state.notify_all();
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
     * Checks the pending/active fields explicitly (rather than comparing the
     * whole packed value to zero) so that this remains accurate regardless of
     * whether @ref STOP_BIT happens to be set.
     *
     * @note This does not prevent new tasks from being submitted concurrently,
     *       so the function may block for longer than expected in that case.
     */
    void waitAllTasksCompleted() {
        uint64_t current_state = task_state.load(std::memory_order_acquire);
        while (pendingFromState(current_state) != 0 || activeFromState(current_state) != 0) {
            task_state.wait(current_state, std::memory_order_relaxed);
            current_state = task_state.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief Signals all workers to stop, joins them, and drains the task queue.
     *
     * Idempotent: calling @ref shutdown more than once is safe (`fetch_or` on
     * an already-set bit is a no-op, and joining an already-joined
     * `std::thread` is skipped via `joinable()`). Any tasks that were
     * enqueued but not yet started are dequeued and discarded (their
     * associated `std::future` will never become ready); the pending counter
     * is decremented to match each one, so @ref pendingCount and @ref isIdle
     * are accurate once this returns.
     *
     * @warning Do not call @ref enqueue after @ref shutdown.
     */
    void shutdown() {
        // Set the shutdown flag directly inside task_state instead of a
        // separate flag. Because every worker parks via
        // task_state.wait(old_value), and wait() is specified to return
        // immediately (without blocking) if the atomic's current value no
        // longer equals `old_value`, this single fetch_or is guaranteed to
        // either be seen by a worker before it parks, or to make its very
        // next wait() call return immediately — there is no window in which
        // a worker can go to sleep and never notice. That means a single
        // notify_all() suffices; there is no need to repeatedly overwrite
        // (and thereby corrupt) the real pending/active counts just to force
        // a wakeup, the way the previous implementation did.
        task_state.fetch_or(STOP_BIT, std::memory_order_release);
        task_state.notify_all();

        // Join the worker threads cleanly to guarantee execution termination.
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        // Clean up trailing allocations to prevent memory leaks. Every task
        // still sitting in the queue here was enqueued (and thus already
        // counted in the pending field) but never claimed by a worker, since
        // all workers have already exited above. Decrement pending to match
        // as each one is discarded, so pendingCount()/isIdle() correctly
        // reflect reality once shutdown() returns — otherwise a task that
        // was queued but never picked up would leave task_state permanently
        // reporting outstanding work that no longer exists.
        MoveOnlyTask abandoned_task;
        while (task_queue.dequeue(abandoned_task)) {
            // Intentionally letting 'abandoned_task' go out of scope drops
            // the lambda, destroying its state and discarding the task.
            task_state.fetch_sub(PENDING_ONE, std::memory_order_acq_rel);
        }
        task_state.notify_all();
    }

    // ── Observers ─────────────────────────────────────────────────────────────

    /**
     * @brief Returns `true` if there are no pending or active tasks.
     * @return `true` when both the pending and active counters are zero,
     *         regardless of whether @ref STOP_BIT is set.
     */
    bool isIdle() const {
        uint64_t s = task_state.load(std::memory_order_acquire);
        return pendingFromState(s) == 0 && activeFromState(s) == 0;
    }

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
 * `maxThreads` (the runtime-detected hardware concurrency,
 * typically from std::thread::hardware_concurrency()) capped at
 * `GlobalConcurrency::MAX_USEFUL_THREADS` (a compile-time upper bound defined
 * in GlobalConcurrency.h to avoid over-subscription on machines with very high
 * core counts).
 *
 * @return Reference to the singleton ThreadPool. The reference remains valid
 *         for the lifetime of the process.
 */
inline ThreadPool& getStaticThreadPool() {
    unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());
    static ThreadPool instance(
        std::min({ static_cast<size_t>(maxThreads),
                   GlobalConcurrency::MAX_USEFUL_THREADS }));
    return instance;
}

#endif // THREAD_POOL_H
