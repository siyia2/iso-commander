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

    // Replace separate pending/active atomics with a single combined
    // counter to eliminate the race window between decrementing pending and
    // incrementing active. The upper 32 bits hold pending_tasks and the lower
    // 32 bits hold active_tasks, updated atomically in a single fetch_add.
    //
    // Encoding:  state = (pending << 32) | active
    //
    // When a worker picks up a task it does a single CAS-like fetch_add that
    // simultaneously decrements pending and increments active:
    //   fetch_add( (-1 << 32) + 1 )  →  pending--, active++
    // When a task completes, active is simply decremented:
    //   fetch_add(-1)
    // On enqueue, pending is incremented:
    //   fetch_add(1 << 32)
    //
    // waitAllTasksCompleted checks state == 0, which can only be true when
    // BOTH halves are zero, with no transient (0,0) window.
    alignas(64) std::atomic<uint64_t> task_state{0};  // (pending<<32)|active

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    mutable std::mutex mutex;
    std::condition_variable cv;

    std::atomic<bool> stop{false};

    // Track the number of sleeping threads to implement a smarter
    // wake-up strategy, but only notify_all on the very first enqueue when all
    // threads are sleeping, preventing repeated thundering-herd wakes when
    // multiple tasks are enqueued in rapid succession.
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    // ---- helpers --------------------------------------------------------

    uint64_t pendingFromState(uint64_t s) const { return s >> 32; }
    uint64_t activeFromState (uint64_t s) const { return s & 0xFFFFFFFFULL; }

    // ---- worker ---------------------------------------------------------

    void workerThread() {
        while (true) {
            std::function<void()> task;

            if (task_queue.dequeue(task)) {
                // Atomically decrement pending and increment active
                // in a single operation — no window where both are zero while
                // a task is actually in flight.
                task_state.fetch_add(
                    static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE,
                    std::memory_order_acq_rel);

                // Wrap task execution in try/catch so that a throwing
                // task cannot propagate out of the worker thread and call
                // std::terminate. The active counter is still decremented
                // correctly via the RAII guard below regardless of exceptions.
                try {
                    task();
                } catch (...) {
                    // Silently swallow — callers observe exceptions via their
                    // std::future (the packaged_task stores the exception there).
                }

                // Decrement active; notify waiters if everything is now idle.
                uint64_t prev = task_state.fetch_sub(ACTIVE_ONE,
                    std::memory_order_acq_rel);
                if (prev == ACTIVE_ONE) {          // we were the last active task
                    std::unique_lock<std::mutex> lk(mutex);
                    cv.notify_all();
                }
                continue;
            }

            // Check stop before sleeping.
            if (stop.load(std::memory_order_acquire)) {
                // Drain any remaining tasks before exiting.
                while (task_queue.dequeue(task)) {
                    task_state.fetch_add(
                        static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE,
                        std::memory_order_acq_rel);
                    try { task(); } catch (...) {}
                    uint64_t prev = task_state.fetch_sub(ACTIVE_ONE,
                        std::memory_order_acq_rel);
                    if (prev == ACTIVE_ONE) {
                        std::unique_lock<std::mutex> lk(mutex);
                        cv.notify_all();
                    }
                }
                return;
            }

            // Go to sleep.
            {
                std::unique_lock<std::mutex> lock(mutex);

                // Double-check stop flag after acquiring mutex to avoid
                // missing a shutdown signal that arrived between the previous
                // check and lock acquisition.
                if (stop.load(std::memory_order_acquire)) {
                    while (task_queue.dequeue(task)) {
                        task_state.fetch_add(
                            static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE,
                            std::memory_order_acq_rel);
                        try { task(); } catch (...) {}
                        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE,
                            std::memory_order_acq_rel);
                        if (prev == ACTIVE_ONE) cv.notify_all();
                    }
                    return;
                }

                sleeping_threads.fetch_add(1, std::memory_order_relaxed);

                cv.wait(lock, [this] {
                    return pendingFromState(
                               task_state.load(std::memory_order_acquire)) > 0
                           || stop.load(std::memory_order_acquire);
                });

                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);

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
        // Wait for all pending/active tasks before signalling shutdown.
        // Note: this can deadlock if tasks submit more tasks and then join
        // their futures — a fundamental limitation of any pool destructor.
        waitAllTasksCompleted();

        stop.store(true, std::memory_order_release);
        cv.notify_all();

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Eliminate the triple-allocation (bind wrapper + packaged_task +
    // queue node) by building the lambda directly inside make_shared so that
    // only two heap allocations are needed: the shared_ptr control block
    // (fused with the packaged_task via make_shared) and the queue node.
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        // Capture arguments in a lambda directly, avoiding std::bind.
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        std::future<return_type> result = task->get_future();

        // Increment pending before pushing to the queue so that
        // waitAllTasksCompleted never misses this task.
        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        task_queue.enqueue([task]() { (*task)(); });

        // FIX #7: Wake strategy — notify_all only when all threads are
        // sleeping (first task of a burst), otherwise notify_one to avoid
        // repeatedly hammering all threads on each enqueue.
        size_t sleeping = sleeping_threads.load(std::memory_order_relaxed);
        if (sleeping == num_threads) {
            cv.notify_all();
        } else {
            cv.notify_one();
        }

        return result;
    }

    // atomic, which has no transient false-zero window.
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

    void waitAndStop() {
        waitAllTasksCompleted();
        stop.store(true, std::memory_order_release);
        cv.notify_all();
    }
};

#endif // THREAD_POOL_H
