// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <thread>

// Project Headers
#include "isocmd/threadpool/threadpool.h"

/**
 * @brief Executes one dequeued task and releases its "active" slot.
 *
 * Runs @p task, swallowing any exception (tasks submitted through
 * enqueue() already route exceptions into their std::promise, so nothing
 * should escape; this keeps a stray throw from killing a worker thread).
 * Afterwards the active counter in @c task_state is decremented, and if
 * that left the pool with no active and no pending work, all waiters
 * (e.g. in waitAllTasksCompleted()) are woken.
 *
 * @param task The task to run; may be empty, in which case only the
 *             counter bookkeeping is performed.
 * @pre The caller has already moved one unit from "pending" to "active"
 *      in @c task_state for this task.
 */
void ThreadPool::runTask(MoveOnlyTask &task) {
    if (task) {
        try {
            task();
        } catch (...) {
        }
    }

    uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);

    if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
        task_state.notify_all();
    }
}

/**
 * @brief Main loop of every worker thread.
 *
 * Each iteration:
 *  1. Returns if the stop bit is set in @c task_state.
 *  2. Parks on @c task_state.wait() while no task is pending, re-checking
 *     the stop bit after every wakeup.
 *  3. Claims a task by CAS-ing @c task_state from "pending" to "active"
 *     (pause/yield and retry if another worker won the race).
 *  4. Dequeues and runs the task via runTask(). If the queue is
 *     unexpectedly empty (the enqueuer bumped the counter before the node
 *     became visible), the claim is rolled back, waiters are notified if
 *     the pool became idle, and the thread yields before retrying.
 *
 * Lock-free: all coordination goes through the single atomic @c task_state.
 */
void ThreadPool::workerThread() {
    while (true) {
        uint64_t current_state = task_state.load(std::memory_order_acquire);

        if (stopFromState(current_state)) {
            return;
        }

        while (pendingFromState(current_state) == 0) {
            task_state.wait(current_state, std::memory_order_relaxed);
            current_state = task_state.load(std::memory_order_acquire);
            if (stopFromState(current_state))
                return;
        }

        uint64_t expected = current_state;
        uint64_t desired = current_state - PENDING_ONE + ACTIVE_ONE;

        if (!task_state.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
            continue;
        }

        MoveOnlyTask task;
        if (task_queue.dequeue(task)) {
            runTask(task);
        } else {
            uint64_t prev =
                task_state.fetch_add(PENDING_ONE - ACTIVE_ONE, std::memory_order_acq_rel);
            if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
                task_state.notify_all();
            }
            std::this_thread::yield();
        }
    }
}

/**
 * @brief Constructs the pool and starts @p n worker threads.
 *
 * @param n Number of worker threads to spawn.
 * @throws std::invalid_argument if @p n is zero.
 */
ThreadPool::ThreadPool(std::size_t n) : num_threads(n) {
    if (n == 0)
        throw std::invalid_argument("ThreadPool: n > 0 required");
    workers.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

/**
 * @brief Stops the pool: calls shutdown(), joining all workers.
 */
ThreadPool::~ThreadPool() {
    shutdown();
}

/**
 * @brief Blocks until no task is pending or running.
 *
 * Sleeps on @c task_state.wait() and re-evaluates after each notification.
 * Does not stop the pool; new tasks may still be enqueued afterwards.
 *
 * @note Returns once the pool is idle at the moment of the check; tasks
 *       enqueued concurrently by other threads may still arrive later.
 */
void ThreadPool::waitAllTasksCompleted() {
    uint64_t current_state = task_state.load(std::memory_order_acquire);
    while (pendingFromState(current_state) != 0 || activeFromState(current_state) != 0) {
        task_state.wait(current_state, std::memory_order_relaxed);
        current_state = task_state.load(std::memory_order_acquire);
    }
}

/**
 * @brief Stops all workers and discards tasks that never started.
 *
 * Sets the stop bit in @c task_state and wakes every parked worker, then
 * joins them. Tasks already running finish; tasks still queued are
 * dequeued and dropped (their pending count is subtracted), so their
 * futures never become ready. Safe to call more than once.
 */
void ThreadPool::shutdown() {
    task_state.fetch_or(STOP_BIT, std::memory_order_release);
    task_state.notify_all();

    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    MoveOnlyTask abandoned_task;
    while (task_queue.dequeue(abandoned_task)) {
        task_state.fetch_sub(PENDING_ONE, std::memory_order_acq_rel);
    }
    task_state.notify_all();
}

/**
 * @brief Returns the process-wide ThreadPool singleton.
 *
 * Created on first use (thread-safe function-local static) with
 * `max(2, hardware_concurrency())` workers, capped at
 * GlobalThreadCaps::MAX_USEFUL_THREADS.
 *
 * @return Reference to the shared pool; valid until program exit.
 */
ThreadPool &getStaticThreadPool() {
    unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());
    static ThreadPool instance(
        std::min({static_cast<std::size_t>(maxThreads), GlobalThreadCaps::MAX_USEFUL_THREADS}));
    return instance;
}
