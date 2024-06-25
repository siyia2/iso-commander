#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global thread lock-free pool for async tasks that includes work stealing
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T value = T()) : data(std::move(value)), next(nullptr) {}
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
        Node* current = head.load(std::memory_order_acquire);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }

    void enqueue(T value) {
        Node* newNode = nullptr;
        try {
            newNode = new Node(std::move(value));
        } catch (...) {
            throw;
        }
        while (true) {
            Node* oldTail = tail.load(std::memory_order_relaxed);
            Node* next = oldTail->next.load(std::memory_order_relaxed);
            if (next == nullptr) {
                if (oldTail->next.compare_exchange_weak(next, newNode,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed)) {
                    tail.compare_exchange_strong(oldTail, newNode,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.compare_exchange_strong(oldTail, next,
                                             std::memory_order_release,
                                             std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            Node* oldHead = head.load(std::memory_order_relaxed);
            Node* next = oldHead->next.load(std::memory_order_relaxed);
            if (next == nullptr) {
                return false;
            }
            if (head.compare_exchange_weak(oldHead, next,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                result = std::move(next->data);
                delete oldHead;
                return true;
            }
        }
    }

    bool isEmpty() const {
        return head.load(std::memory_order_acquire)->next.load(std::memory_order_acquire) == nullptr;
    }

    bool steal(T& result) {
        return dequeue(result);
    }
};

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false), nextQueue(0) {
        for (size_t i = 0; i < numThreads; ++i) {
            queues.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>>>());
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i);
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
            std::unique_lock<std::mutex> lock(mutex);
            if (stop) {
                throw std::runtime_error("Enqueue on stopped ThreadPool");
            }
            size_t queueIndex = nextQueue;
            nextQueue = (nextQueue + 1) % queues.size();
            queues[queueIndex]->enqueue([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stop = true;
        }
        condition.notify_all();

        // Wait for all tasks to complete
        for (auto& queue : queues) {
            std::function<void()> task;
            while (queue->dequeue(task)) {
                task();
            }
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    void workerThread(size_t id) {
        while (true) {
            std::function<void()> task;
            bool gotTask = false;
            
            // Try to get a task from our own queue
            gotTask = queues[id]->dequeue(task);
            
            // If no task in our queue, try to steal from others
            if (!gotTask) {
                for (size_t i = 0; i < queues.size(); ++i) {
                    if (i != id && queues[i]->steal(task)) {
                        gotTask = true;
                        break;
                    }
                }
            }
            
            // If we got a task, execute it
            if (gotTask) {
                task();
                continue;
            }
            
            // If no task was found, wait for notification
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this, id] { 
                return stop || !queues[id]->isEmpty() || 
                       std::any_of(queues.begin(), queues.end(), 
                           [](const auto& q) { return !q->isEmpty(); });
            });
            
            if (stop && std::all_of(queues.begin(), queues.end(), 
                [](const auto& q) { return q->isEmpty(); })) {
                return;
            }
        }
    }

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;
    std::mutex mutex;
    std::condition_variable condition;
    bool stop;
    size_t nextQueue; // Round-robin task distribution
};

#endif // THREAD_POOL_H
