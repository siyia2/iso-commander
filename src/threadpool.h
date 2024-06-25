#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global lock-free threadpool woth work stealing
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i); // Start worker threads with an index
            task_queues.emplace_back(); // Create a task queue for each worker
        }
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        
        {
            std::lock_guard<std::mutex> lock(mutex); // Lock the task queue
            if (stop) {
                throw std::runtime_error("Enqueue on stopped ThreadPool");
            }
            task_queues[current_index].push_back([task]() { (*task)(); }); // Enqueue the task to the current thread's queue
        }
        condition.notify_one(); // Notify a worker thread
        return res;
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex); // Lock the task queue
            stop = true; // Set stop flag to true
        }
        condition.notify_all(); // Notify all worker threads to stop
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join(); // Join all worker threads
            }
        }
    }

private:
    void workerThread(size_t index) {
        current_index = index; // Set the current thread index

        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex); // Lock the task queue
                condition.wait(lock, [this] { 
                    return stop || !task_queues[current_index].empty(); 
                }); // Wait for a task or stop signal

                if (stop && task_queues[current_index].empty()) {
                    return; // Exit if stop is true and no tasks are left
                }

                if (!task_queues[current_index].empty()) {
                    task = std::move(task_queues[current_index].front());
                    task_queues[current_index].pop_front(); // Dequeue a task
                }
            }

            if (!task) {
                // Attempt to steal a task
                size_t numThreads = workers.size();
                for (size_t i = 1; i < numThreads; ++i) {
                    size_t steal_index = (index + i) % numThreads;
                    {
                        std::unique_lock<std::mutex> lock(mutex); // Lock the task queue
                        if (!task_queues[steal_index].empty()) {
                            task = std::move(task_queues[steal_index].front());
                            task_queues[steal_index].pop_front(); // Steal a task
                            break;
                        }
                    }
                }
            }

            if (task) {
                task(); // Execute the task
            }
        }
    }

    std::vector<std::thread> workers; // Worker threads
    std::vector<std::deque<std::function<void()>>> task_queues; // Task queues for each worker
    std::mutex mutex; // Mutex for task queue
    std::condition_variable condition; // Condition variable for task queue
    std::atomic<bool> stop; // Stop flag
    size_t current_index; // Index of the current thread
};

#endif // THREAD_POOL_H
