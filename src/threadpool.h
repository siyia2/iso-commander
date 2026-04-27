// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "./headers.h"

/**
 * @brief A lock-free, concurrent queue implementation.
 * @details Uses C++20 atomic shared_ptr to handle the memory reclamation and 
 * avoids the ABA problem. This implementation ensures that only the thread 
 * winning the CAS takes ownership of the data.
 * @tparam T The type of elements stored in the queue.
 */
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

    /**
     * @brief Enqueues an item into the queue.
     * @param value The item to be added.
     */
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

    /**
     * @brief Dequeues an item from the queue.
     * @param result Reference to store the popped item.
     * @return True if successful, false if queue is empty.
     */
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
                    // FIX: We must only move the data AFTER winning the CAS to avoid 
                    // multiple threads moving from the same node or double-freeing.
                    if (head.compare_exchange_weak(first, next,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                        result = std::move(next->data);
                        return true;
                    }
                }
            }
        }
    }
};

/**
 * @brief High-performance ThreadPool using a lock-free task queue.
 * @details Manages worker threads and utilizes an atomic 64-bit state 
 * to track both pending and active tasks for synchronization.
 */
class ThreadPool {
private:
    const size_t num_threads;
    alignas(64) std::atomic<uint64_t> task_state{0}; 

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    uint64_t pendingFromState(uint64_t s) const { return s >> 32; }
    
    void notifyIfIdle(uint64_t prevState) {
        // If the last active task just finished and no pending tasks exist
        if (prevState == ACTIVE_ONE) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    void runTask(std::function<void()>& task) {
        // Atomic transition: decrement pending, increment active
        task_state.fetch_add(static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE, std::memory_order_acq_rel);
        
        if (task) {
            try { task(); } catch (...) { /* Suppress worker leak */ }
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

            if (stop.load(std::memory_order_acquire)) return;

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
    /**
     * @brief Construct a new Thread Pool.
     * @param n Number of worker threads.
     */
    explicit ThreadPool(size_t n) : num_threads(n) {
        if (n == 0) throw std::invalid_argument("ThreadPool: n > 0 required");
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    /**
     * @brief Destructor ensures all tasks are completed and threads joined.
     */
    ~ThreadPool() {
        // 1. Wait for current workload to finish naturally
        waitAllTasksCompleted();
        
        // 2. Use the unified shutdown logic
        shutdown();
    }

    /**
     * @brief Submits a task to the pool.
     * @return std::future to retrieve the result later.
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

        if (sleeping_threads.load(std::memory_order_relaxed) > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_one();
        }

        return result;
    }

    /**
     * @brief Blocks until the queue is empty and all active tasks finish.
     */
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }
    
    /**
     * @brief Unified shutdown logic to stop workers and clear memory.
     */
    void shutdown() {
		// 1. Signal stop. If already signaled, do nothing.
		if (stop.exchange(true, std::memory_order_acq_rel)) return;

		// 2. Wake up all sleeping threads so they can see the stop signal
		{
			std::lock_guard<std::mutex> lock(mutex);
			cv.notify_all();
		}

		// 3. Join workers. Now the heap is safe from background access.
		for (auto& t : workers) {
			if (t.joinable()) t.join();
		}
		
		// 4. (Optional) Clear the queue function wrappers
		std::function<void()> residual;
		while (task_queue.dequeue(residual)) { residual = nullptr; }
	}

    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }
    size_t threadCount() const { return num_threads; }
};


/**
 * @brief Retrieves a singleton instance of the ThreadPool.
 */
inline ThreadPool& getStaticThreadPool() {
    static ThreadPool* instance = new ThreadPool([] {
        return std::min({static_cast<size_t>(maxThreads), MAX_USEFUL_THREADS});
    }());
    
    [[maybe_unused]] static bool registered = []() {
        std::atexit([]() { 
            // This will only run if the user didn't 
            // manually call shutdown() in main().
            instance->shutdown(); 
        });
        return true;
    }();
    
    return *instance;
}

#endif // THREAD_POOL_H
