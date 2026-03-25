// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "./headers.h"

// ========================= LOCK-FREE QUEUE =========================
// Michael-Scott lock-free queue (without node reuse to avoid ABA)
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        std::atomic<Node*> next;
        T data;
        Node() : next(nullptr) {}
        Node(T&& val) : next(nullptr), data(std::move(val)) {}
    };

    alignas(64) std::atomic<Node*> head;
    alignas(64) std::atomic<Node*> tail;

public:
    LockFreeQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeQueue() {
        Node* node = head.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    void enqueue(T value) {
        Node* new_node = new Node(std::move(value));
        while (true) {
            Node* last = tail.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);
            if (last != tail.load(std::memory_order_acquire))
                continue;
            if (next == nullptr) {
                if (last->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    tail.compare_exchange_weak(last, new_node,
                        std::memory_order_release,
                        std::memory_order_acquire);
                    return;
                }
            } else {
                tail.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_acquire);
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            Node* first = head.load(std::memory_order_acquire);
            Node* last  = tail.load(std::memory_order_acquire);
            Node* next  = first->next.load(std::memory_order_acquire);
            if (first != head.load(std::memory_order_acquire))
                continue;
            if (first == last) {
                if (next == nullptr)
                    return false;
                tail.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_acquire);
            } else {
                if (head.compare_exchange_weak(first, next,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    result = std::move(next->data);
                    delete first;
                    return true;
                }
            }
        }
    }

    bool isEmpty() const {
        Node* h = head.load(std::memory_order_acquire);
        return h->next.load(std::memory_order_acquire) == nullptr;
    }
};

// ========================= THREAD POOL =========================
class ThreadPool {
private:
    const size_t num_threads;

    // Single combined counter encoding both pending and active task counts.
    //
    // Encoding:  task_state = (pending << 32) | active
    //
    //   enqueue:              fetch_add(PENDING_ONE)
    //   worker picks up task: fetch_add(-PENDING_ONE + ACTIVE_ONE)  [atomic swap]
    //   task completes:       fetch_sub(ACTIVE_ONE)
    //
    // This eliminates the race window that existed when pending and active were
    // separate atomics: there is no moment where both halves transiently read
    // zero while a task is actually in flight.
    //
    // waitAllTasksCompleted checks task_state == 0, which is only true when
    // BOTH pending == 0 AND active == 0 simultaneously.
    alignas(64) std::atomic<uint64_t> task_state{0};  // (pending<<32)|active

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    mutable std::mutex mutex;
    std::condition_variable cv;

    std::atomic<bool> stop{false};

    // Approximate count of threads currently sleeping on cv.
    // Used by enqueue() to decide between notify_one and notify_all.
    // Only notify_all when ALL threads are sleeping (first task of a burst)
    // to avoid repeated thundering-herd wakes on rapid bulk enqueues.
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    // ---- helpers --------------------------------------------------------

    uint64_t pendingFromState(uint64_t s) const { return s >> 32; }
    uint64_t activeFromState (uint64_t s) const { return s & 0xFFFFFFFFULL; }

    // Notify waitAllTasksCompleted() that the pool may now be idle.
    // Called after every task completion.
    //
    // prev is the task_state value BEFORE the completing fetch_sub(ACTIVE_ONE).
    // prev == ACTIVE_ONE means: active was 1 (now 0) AND pending was 0.
    // That is the exact moment the pool transitions to fully idle.
    //
    // The notify is intentionally issued WITHOUT holding the mutex.
    // condition_variable::notify_all() does not require the mutex to be held —
    // holding it would cause the woken waiter to immediately re-contend for
    // the lock, adding unnecessary latency. The memory ordering of the
    // preceding fetch_sub(acq_rel) ensures the state update is visible to the
    // waiter before it re-checks its predicate under its own lock.
    void notifyIfIdle(uint64_t prev) {
        if (prev == ACTIVE_ONE) {
            cv.notify_all();   // no lock held — correct and faster
        }
    }

    // Execute one task that has already been dequeued.
    // Atomically converts it from pending→active, runs it, then active→done.
    //
    // Removed the early-return stop check that previously abandoned
    // dequeued tasks without executing them. Any task that has been dequeued
    // has a live std::future waiting on its result — silently dropping it
    // destroys the packaged_task without fulfilling the promise, causing
    // std::future_error (broken promise) in the caller, and std::terminate
    // if that exception propagates through a noexcept frame (e.g. a
    // destructor). The stop flag governs the worker loop (whether to wait for
    // MORE work), not whether to honour already-queued work.
    void runTask(std::function<void()>& task) {
        // Atomically decrement pending and increment active in one operation.
        task_state.fetch_add(
            static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE,
            std::memory_order_acq_rel);

        try {
            task();
        } catch (...) {}

        // Decrement active and wake any idle-waiters if pool is now empty.
        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE,
            std::memory_order_acq_rel);
        notifyIfIdle(prev);
    }

    // ---- worker ---------------------------------------------------------

    void workerThread() {
        while (true) {
            std::function<void()> task;

            if (task_queue.dequeue(task)) {
                runTask(task);
                continue;
            }

            // No task available — check stop before sleeping.
            if (stop.load(std::memory_order_acquire)) {
                // Drain any tasks that arrived just before stop was set.
                while (task_queue.dequeue(task))
                    runTask(task);
                return;
            }

            // Go to sleep until a task or stop signal arrives.
            {
                std::unique_lock<std::mutex> lock(mutex);

                // Re-check stop under the lock to close the window between
                // the check above and lock acquisition.
                if (stop.load(std::memory_order_acquire)) {
                    while (task_queue.dequeue(task))
                        runTask(task);
                    return;
                }

                sleeping_threads.fetch_add(1, std::memory_order_relaxed);

                // Predicate uses pending count from task_state (single atomic
                // read) rather than isEmpty() (two non-atomic loads) to avoid
                // spurious wakes caused by a racy isEmpty() result.
                cv.wait(lock, [this] {
                    return pendingFromState(
                               task_state.load(std::memory_order_acquire)) > 0
                           || stop.load(std::memory_order_acquire);
                });

                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);

                // Exit only if stop is set AND nothing is left to process.
                if (stop.load(std::memory_order_acquire) &&
                    pendingFromState(task_state.load(std::memory_order_acquire)) == 0) {
                    return;
                }
            }
        }
    }

public:
    explicit ThreadPool(size_t n)
        : num_threads(n)
    {
        if (n == 0) {
            throw std::invalid_argument(
                "ThreadPool: number of threads must be greater than 0");
        }
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    ~ThreadPool() {
        // Drain all pending/active work before shutting down threads.
        // Note: deadlocks if tasks circularly wait on each other's futures.
        waitAllTasksCompleted();

        stop.store(true, std::memory_order_release);
        cv.notify_all();

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        // Capture args in a lambda (avoids std::bind) and fuse the
        // packaged_task with its control block via make_shared — two heap
        // allocations total instead of three.
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        std::future<return_type> result = task->get_future();

        // Increment pending BEFORE pushing to queue so waitAllTasksCompleted
        // cannot observe a transient idle state for this task.
        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        task_queue.enqueue([task]() { (*task)(); });

        // Wake strategy: notify_all only when every thread is sleeping
        // (i.e., the first task of a burst hitting an idle pool).
        // For subsequent enqueues while threads are already running,
        // notify_one is sufficient and avoids unnecessary context switches.
        if (sleeping_threads.load(std::memory_order_relaxed) == num_threads) {
            cv.notify_all();
        } else {
            cv.notify_one();
        }

        return result;
    }

    // FIX: Removed the `|| stop` short-circuit from the wait predicate.
    // Previously, if waitAndStop() set stop=true and then the destructor
    // called waitAllTasksCompleted(), the predicate would return true
    // immediately (because stop==true) even while tasks were still active,
    // causing the destructor to proceed to join() threads before their
    // current tasks had finished — resulting in broken promises for any
    // live futures still waiting on those tasks.
    // The correct invariant: only return when task_state is truly zero
    // (both pending AND active counts are 0), regardless of stop state.
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }

    bool isIdle() const {
        return task_state.load(std::memory_order_acquire) == 0;
    }

    size_t threadCount()   const { return num_threads; }

    size_t pendingCount()  const {
        return pendingFromState(task_state.load(std::memory_order_acquire));
    }

    size_t activeCount()   const {
        return activeFromState(task_state.load(std::memory_order_acquire));
    }

    size_t sleepingCount() const {
        return sleeping_threads.load(std::memory_order_relaxed);
    }

    // waitAndStop() is retained for external callers that need it,
    // but the atexit registration in filtering.cpp that called it has been
    // removed. The destructor's own waitAllTasksCompleted() call is
    // sufficient for clean shutdown — a second call from atexit during
    // static destruction raced with live futures held on stack frames that
    // had already been torn down.
    void waitAndStop() {
        waitAllTasksCompleted();
        stop.store(true, std::memory_order_release);
        cv.notify_all();
    }
};

#endif // THREAD_POOL_H
