// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++ Standard Library Headers
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
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
 * @brief A high-performance, fixed-size, move-only type-erased callable wrapper.
 *
 * @details This component replaces standard copyable type erasers to fully eliminate
 * runtime heap allocations and indirect reference counting. It establishes an internal
 * stack-allocated arena where lambda closures can be initialized directly.
 *
 * Technical Specs & Constraints:
 * - Total Object Size: Exactly 128 bytes (2 cache lines), matching typical CPU architecture
 * alignment steps to prevent cache line splitting or false sharing.
 * - Capture Limit: Up to 120 bytes of lambda context can be retained directly on the stack.
 * If a lambda closure exceeds 120 bytes, a compilation error is triggered via static_assert.
 * - Move Semantics: Exclusively move-only via type-safe placement-new constructors.
 * Copying is explicitly deleted to avoid hidden allocation logic or deep cloning.
 */
class MoveOnlyTask {
private:
    /// @brief Storage size allocated for the inline lambda capture arena.
    /// @note Set to 120 bytes so that 120 + 8 (vtable pointer) = exactly 128 bytes (2 cache lines).
    static constexpr std::size_t StorageSize = 120;

    /**
     * @brief Internal execution and lifecycle function pointer table.
     * * @details Maps type-erased instance operations to specific underlying closure implementations
     * without needing standard inheritance hierarchies or dynamic dispatch.
     */
    struct VTable {
        /// @brief Function pointer matching signature to execute the erased callable context.
        void (*call)(void*);
        /// @brief Function pointer matching signature to clean up the erased context destructively.
        void (*destroy)(void*) noexcept;
        /// @brief Function pointer matching signature to move the context from one memory slot to another.
        void (*move)(void*, void*) noexcept;
    };

    /// @brief Inline stack arena aligned to the platform's maximum structural boundary.
    alignas(std::max_align_t) char storage[StorageSize];

    /// @brief Current pointer into the global static VTable matrix tracking the assigned type.
    const VTable* vtable{nullptr};

    /**
     * @brief Erased wrapper that directly invokes the underlying typed callable.
     * @tparam F The true, decayed type of the erased callable.
     * @param ptr Pointer to the raw memory region storing the instance of type F.
     */
    template <typename F>
    static void call_impl(void* ptr) {
        (*static_cast<F*>(ptr))();
    }

    /**
     * @brief Erased wrapper that calls the destructor of the underlying typed callable.
     * @tparam F The true, decayed type of the erased callable.
     * @param ptr Pointer to the raw memory region storing the instance of type F.
     */
    template <typename F>
    static void destroy_impl(void* ptr) noexcept {
        static_cast<F*>(ptr)->~F();
    }

    /**
     * @brief Erased wrapper that invokes the move constructor via placement-new.
     * @tparam F The true, decayed type of the erased callable.
     * @param dst Target storage pointer where the new object will be constructed.
     * @param src Source storage pointer from which content will be moved out.
     */
    template <typename F>
    static void move_impl(void* dst, void* src) noexcept {
        ::new (dst) F(std::move(*static_cast<F*>(src)));
    }

    /**
     * @brief Statically allocated explicit implementation matrix mapping the lifecycle of type F.
     * @tparam F The true, decayed type of the erased callable.
     */
    template <typename F>
    static constexpr VTable vtable_for = { &call_impl<F>, &destroy_impl<F>, &move_impl<F> };

public:
    /** @brief Constructs an uninitialized, empty task wrapper. */
    MoveOnlyTask() noexcept = default;

    /**
     * @brief Template constructor for any callable object.
     *
     * @details Uses placement-new to initialize the incoming callable directly inside
     * the internal storage array. Evaluates boundaries at compile time.
     *
     * @tparam F Type of the incoming callable object.
     * @param f Rvalue reference to the source callable context.
     */
    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyTask>>>
    MoveOnlyTask(F&& f) {
        using DecayedF = std::decay_t<F>;
        static_assert(sizeof(DecayedF) <= StorageSize,
            "Task capture size exceeds fixed-size buffer allocation.");
        static_assert(alignof(DecayedF) <= alignof(std::max_align_t),
            "Task alignment exceeds buffer alignment constraints.");

        ::new (static_cast<void*>(storage)) DecayedF(std::forward<F>(f));
        vtable = &vtable_for<DecayedF>;
    }

    /** @brief Cleans up internal allocations by invoking the type-specific destructor. */
    ~MoveOnlyTask() noexcept {
        reset();
    }

    // Deleted Copy Semantics
    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    /** @brief Move constructor. Transfers ownership of the memory state. */
    MoveOnlyTask(MoveOnlyTask&& other) noexcept {
        move_from(std::move(other));
    }

    /** @brief Move assignment operator. Cleans up current state and links incoming task. */
    MoveOnlyTask& operator=(MoveOnlyTask&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    /** @brief Invokes the underlying type-erased callable object. */
    void operator()() {
        if (vtable) {
            vtable->call(static_cast<void*>(storage));
        }
    }

    /** @brief Boolean conversion operator to check if the instance contains a valid task. */
    explicit operator bool() const noexcept {
        return vtable != nullptr;
    }

    /** @brief Destroys the current callable context and resets the task to an uninitialized state. */
    void reset() noexcept {
        if (vtable) {
            vtable->destroy(storage);
            vtable = nullptr;
        }
    }

private:
    /**
     * @brief Handles internal memory migration logic for move mechanics.
     * @param other The rvalue context to transfer state from.
     */
    void move_from(MoveOnlyTask&& other) noexcept {
        vtable = other.vtable;
        if (vtable) {
            vtable->move(storage, other.storage);
            other.reset();
        }
    }
};

/**
 * @brief A thread-safe, non-blocking Michael-Scott Lock-Free Queue optimized for MoveOnlyTask.
 *
 * @details This implementation utilizes C++20 atomic shared_ptr to eliminate the
 * classic ABA problem without requiring a complex garbage collector.
 *
 * Concurrency guarantees:
 * - Safe for any number of concurrent producers (enqueue) and consumers (dequeue)
 * simultaneously.
 * - Progress guarantee: enqueue() and dequeue() are lock-free.
 * - Expected complexity: O(1) amortized per operation under low contention.
 *
 * Key Robustness Features:
 * 1. Inline Data Storage: Tasks are stored directly inside the node structure, eliminating
 * the extra heap allocation and alignment complexities of std::unique_ptr wrappers.
 * 2. Tail-Helping: Includes logic to allow threads to "help" move the tail
 * pointer forward if it lags behind, preventing a single stalled thread
 * from blocking the entire queue.
 *
 * @tparam T The type of elements stored in the queue (optimized for MoveOnlyTask).
 */
template <typename T>
class LockFreeQueue {
private:
    /**
     * @brief Singly-linked block node containing the actual task context payload.
     */
    struct Node {
        /// @brief The raw inline memory footprint holding the typed task data wrapper.
        T data;
        /// @brief Atomic shared ownership reference pointing directly to the succeeding node in sequence.
        std::atomic<std::shared_ptr<Node>> next;

        /** @brief Constructs a blank node acting primarily as a tracking boundary marker. */
        Node() : data(), next(nullptr) {}
        /** @brief Constructs a payload node by moving data seamlessly into the inline structure. */
        Node(T&& val) : data(std::move(val)), next(nullptr) {}
    };

    /// @brief Head reference pointer tracking the dynamic trailing base of the active queue sequence.
    alignas(64) std::atomic<std::shared_ptr<Node>> head;
    /// @brief Tail reference pointer tracking the speculative end boundary where items are appended.
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
     * @param value The item to move into the queue.
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
                    tail.compare_exchange_weak(last, new_node, std::memory_order_release);
                    return;
                }
            } else {
                // Tail is lagging; help move it forward so other threads aren't blocked.
                tail.compare_exchange_weak(last, next, std::memory_order_release);
            }

            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
    }

    /**
     * @brief Atomically dequeues an item.
     * @param result Reference into which the dequeued item is moved on success.
     * @return true if an item was retrieved, false if the queue was empty.
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
                    auto consumed = next;  // next is now the new head
                    if (consumed->data) {
                        result = std::move(consumed->data);
                        consumed->data.reset();
                    }
                    return true;
                }
            }

            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
    }
};

/**
 * @brief High-performance ThreadPool using lock-free allocation-less task management.
 *
 * @details This pool manages a set of worker threads that consume tasks from a
 * LockFreeQueue<MoveOnlyTask>. It utilizes a 64-bit atomic state bitfield (32-bit pending,
 * 32-bit active) to track workload without the overhead of heavy mutex locking.
 */
class ThreadPool {
private:
    /// @brief Configured capacity constraint detailing spawned thread workers.
    const size_t num_threads;

    /// @brief Bits 63-32: Number of tasks in queue. Bits 31-0: Number of tasks currently running.
    alignas(64) std::atomic<uint64_t> task_state{0};

    /// @brief Explicit value bit representation indicating exactly 1 task added to the upper 32-bit field.
    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    /// @brief Explicit value bit representation indicating exactly 1 task added to the lower 32-bit field.
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    /// @brief Native system thread storage handling long-running task workers.
    std::vector<std::thread> workers;
    /// @brief Internal Michael-Scott queue optimized to prevent lock step penalties.
    LockFreeQueue<MoveOnlyTask> task_queue;

    /// @brief Mutex managing synchronization across workers during low workload state tracking.
    std::mutex              mutex;
    /// @brief Condition variable managing signaling between execution boundaries.
    std::condition_variable cv;
    /// @brief Flag indicating termination state changes across global threads.
    std::atomic<bool>       stop{false};
    /// @brief Counter tracking current quantity of workers parked on cv.wait structures.
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    /**
     * @brief Direct helper utility separating the high 32 bits from packed state parameters.
     * @param s The current combined 64-bit status value.
     * @return Transformed task count currently inside the queue container.
     */
    static uint64_t pendingFromState(uint64_t s) { return s >> 32; }

    /**
     * @brief Signals cv structure handlers if task_state evaluates precisely to empty.
     * @param prevState Monitored task_state composition values captured prior to updates.
     */
    void notifyIfIdle(uint64_t prevState) {
        uint64_t newState = prevState - ACTIVE_ONE;
        if (newState == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    /**
     * @brief Transforms active counters, runs task payload contexts, and processes exit tracking flags.
     * @param task Raw task wrapper extracted from the internal lock-free tracking list.
     */
    void runTask(MoveOnlyTask& task) {
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
     * @brief Primary consumer loop routine mapping life sequences for active OS threads.
     */
    void workerThread() {
        while (true) {
            MoveOnlyTask task;

            if (task_queue.dequeue(task)) {
                runTask(task);
                continue;
            }

            if (stop.load(std::memory_order_acquire) &&
                task_state.load(std::memory_order_acquire) == 0) {
                return;
            }

            {
                std::unique_lock<std::mutex> lock(mutex);
                sleeping_threads.fetch_add(1, std::memory_order_relaxed);

                cv.wait(lock, [this] {
                    return pendingFromState(task_state.load(std::memory_order_acquire)) > 0
                        || stop.load(std::memory_order_acquire);
                });

                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

public:
    /**
     * @brief Constructs a ThreadPool and spawns @p n worker threads.
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

    /** @brief Destructor. Blocks until all pending tasks are finished, then joins workers. */
    ~ThreadPool() {
        waitAllTasksCompleted();
        shutdown();
    }

    /**
     * @brief Submits a callable and its arguments to the pool for asynchronous execution.
     * * @tparam F Callable type (function, lambda, functor, etc.).
     * @tparam Args Argument types forwarded to @p f.
     * @param f The callable to invoke on a worker thread.
     * @param args Arguments forwarded to @p f at the time of invocation.
     * @return std::future mapping the evaluation result of the submitted task.
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto promise = std::make_shared<std::promise<return_type>>();
        std::future<return_type> result = promise->get_future();

        task_state.fetch_add(PENDING_ONE, std::memory_order_release);

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

        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_one();
        }

        return result;
    }

    /** @brief Blocks the calling thread until the queue is empty and all workers are idle. */
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }

    /** @brief Signals all workers to exit after draining remaining work, then joins every thread. */
    void shutdown() {
        if (stop.exchange(true, std::memory_order_acq_rel)) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        MoveOnlyTask residual;
        while (task_queue.dequeue(residual)) { residual.reset(); }
    }

    /** @brief Returns true if no tasks are pending or active. */
    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }

    /** @brief Returns the total number of worker threads. */
    size_t threadCount() const { return num_threads; }

    /** @brief Returns the number of tasks currently in the queue (pending, not yet running). */
    uint64_t pendingCount() const { return pendingFromState(task_state.load(std::memory_order_acquire)); }

    /** @brief Returns the number of tasks currently being executed by worker threads. */
    uint64_t activeCount() const { return task_state.load(std::memory_order_acquire) & 0xFFFFFFFFULL; }
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
