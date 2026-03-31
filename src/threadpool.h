// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "./headers.h"


// ========================= LOCK-FREE QUEUE =========================
// Modern C++20 Lock-free queue using atomic shared_ptr
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<std::shared_ptr<Node>> next;
        Node() : next(nullptr) {}
        Node(T&& val) : data(std::move(val)), next(nullptr) {}
    };

    alignas(64) std::atomic<std::shared_ptr<Node>> head;
    alignas(64) std::atomic<std::shared_ptr<Node>> tail;

public:
    LockFreeQueue() {
        auto dummy = std::make_shared<Node>();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    void enqueue(T value) {
        auto new_node = std::make_shared<Node>(std::move(value));
        while (true) {
            std::shared_ptr<Node> last = tail.load(std::memory_order_acquire);
            std::shared_ptr<Node> next = last->next.load(std::memory_order_acquire);

            if (last == tail.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    if (last->next.compare_exchange_weak(next, new_node,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed)) {
                        tail.compare_exchange_weak(last, new_node,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed);
                        return;
                    }
                } else {
                    tail.compare_exchange_weak(last, next,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
                }
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            std::shared_ptr<Node> first = head.load(std::memory_order_acquire);
            std::shared_ptr<Node> last = tail.load(std::memory_order_acquire);
            std::shared_ptr<Node> next = first->next.load(std::memory_order_acquire);

            if (first == head.load(std::memory_order_acquire)) {
                if (first == last) {
                    if (next == nullptr) return false;
                    tail.compare_exchange_weak(last, next, 
                                              std::memory_order_release, 
                                              std::memory_order_relaxed);
                } else {
                    // Copy shared_ptr/function first to ensure stability during CAS
                    T captured_data = next->data; 
                    if (head.compare_exchange_weak(first, next,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                        result = std::move(captured_data);
                        return true;
                    }
                }
            }
        }
    }
};

// ========================= THREAD POOL =========================
class ThreadPool {
private:
    const size_t num_threads;
    alignas(64) std::atomic<uint64_t> task_state{0}; 

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    uint64_t pendingFromState(uint64_t s) const { return s >> 32; }
    uint64_t activeFromState (uint64_t s) const { return s & 0xFFFFFFFFULL; }

    void notifyIfIdle(uint64_t prevState) {
        if (prevState == ACTIVE_ONE) {
            cv.notify_all();
        }
    }

    void runTask(std::function<void()>& task) {
        task_state.fetch_add(static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE, std::memory_order_acq_rel);
        
        if (task) {
            try { task(); } catch (...) {}
        }

        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);
        notifyIfIdle(prev);
    }

    void workerThread() {
        while (true) {
            std::function<void()> task;

            if (task_queue.dequeue(task)) {
                runTask(task);
                continue;
            }

            if (stop.load(std::memory_order_acquire)) {
                if (task_queue.dequeue(task)) {
                    runTask(task);
                    continue;
                }
                return;
            }

            {
                std::unique_lock<std::mutex> lock(mutex);
                sleeping_threads.fetch_add(1, std::memory_order_relaxed);
                cv.wait(lock, [this] {
                    return pendingFromState(task_state.load(std::memory_order_acquire)) > 0 || stop.load(std::memory_order_acquire);
                });
                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

public:
    explicit ThreadPool(size_t n) : num_threads(n) {
        if (n == 0) throw std::invalid_argument("ThreadPool: n > 0 required");
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    ~ThreadPool() {
        waitAllTasksCompleted();

        stop.store(true, std::memory_order_release);
        cv.notify_all();

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        std::function<void()> residual;
        while (task_queue.dequeue(residual)) {
            if (residual) {
                runTask(residual);
            }
        }
    }

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

        if (sleeping_threads.load(std::memory_order_relaxed) > 0) {
            cv.notify_one();
        }

        return result;
    }

    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }

    // Getters for naturalSort and processInput ---
    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }
    size_t threadCount() const { return num_threads; }
};


// Static access helper
inline ThreadPool& getStaticThreadPool() {
    static ThreadPool instance([] {
        constexpr size_t MAX_USEFUL_THREADS = 32;
        return std::min({static_cast<size_t>(maxThreads), MAX_USEFUL_THREADS});
    }());
    return instance;
}

#endif // THREAD_POOL_H
