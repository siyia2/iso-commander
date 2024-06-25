#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global thread pool for async tasks
class ThreadPool {
public:
    // Constructor to initialize the thread pool with a specified number of threads.
    explicit ThreadPool(size_t numThreads) : stop(false) {
        // Spawn worker threads.
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    // Enqueue a task into the thread pool and return a future to track its execution.
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        // Get a future to track the task's result.
        std::future<return_type> res = task->get_future();
        
        {
            // Lock the task queue for thread-safe access.
            std::unique_lock<std::mutex> lock(queueMutex);
            // Check if the thread pool has been stopped.
            if (stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            // Enqueue the task into the task queue.
            tasks.emplace([task]() { (*task)(); });
        }
        // Notify a waiting thread that a new task is available.
        condition.notify_one();
        return res;
    }

    // Destructor to clean up resources.
    ~ThreadPool() {
        {
            // Lock the task queue for thread-safe access.
            std::unique_lock<std::mutex> lock(queueMutex);
            // Set the stop flag to true to signal worker threads to stop.
            stop = true;
        }
        // Notify all worker threads that the thread pool is being shut down.
        condition.notify_all();
        // Join all worker threads to wait for their completion.
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

private:
    // Structure representing a worker thread in the thread pool.
    struct Worker {
        std::queue<std::function<void()>> tasks; // Queue of tasks assigned to this worker.
        std::mutex mutex; // Mutex to synchronize access to the task queue.
    };

    // Function executed by each worker thread.
    void workerThread() {
        Worker worker;
        while (true) {
            std::function<void()> task;
            {
                // Lock the task queue for thread-safe access.
                std::unique_lock<std::mutex> lock(queueMutex);
                // Wait until there's a task available or the thread pool is stopped.
                condition.wait(lock, [this, &worker] {
                    return stop || !tasks.empty() || !worker.tasks.empty();
                });
                // If the thread pool is stopped and no tasks remain, exit the loop.
                if (stop && tasks.empty() && worker.tasks.empty()) {
                    return;
                }
                // Prioritize tasks from the thread pool's task queue.
                if (!tasks.empty()) {
                    task = std::move(tasks.front());
                    tasks.pop();
                } 
                // If the thread's own task queue is not empty, execute a task from it.
                else if (!worker.tasks.empty()) {
                    task = std::move(worker.tasks.front());
                    worker.tasks.pop();
                } 
                // If no tasks are available for this thread, perform work stealing.
                else {
                    for (auto& other_worker : workers_) {
                        std::unique_lock<std::mutex> lock(other_worker.mutex);
                        if (!other_worker.tasks.empty()) {
                            task = std::move(other_worker.tasks.front());
                            other_worker.tasks.pop();
                            break;
                        }
                    }
                }
            }
            // Execute the retrieved task.
            if (task) {
                task();
            }
        }
    }

    std::vector<std::thread> workers; // Vector to store worker threads.
    std::queue<std::function<void()>> tasks; // Queue of tasks for the thread pool.
    std::mutex queueMutex; // Mutex to synchronize access to the task queue.
    std::condition_variable condition; // Condition variable for task synchronization.
    std::atomic<bool> stop; // Atomic flag to indicate whether the thread pool is stopped.

    std::vector<Worker> workers_; // Vector to store worker structures for work stealing.
};

#endif // THREAD_POOL_H
