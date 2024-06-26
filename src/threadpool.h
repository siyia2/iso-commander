#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global lock-free thread pool for async tasks that includes work stealing
template <typename T>
class LockFreeQueue {
private:
    struct alignas(64) Node {
        T data;
        std::atomic<Node*> next;
        Node(T value = T()) : data(std::move(value)), next(nullptr) {}
    };

    alignas(64) std::atomic<Node*> head;
    alignas(64) std::atomic<Node*> tail;

    // Custom memory pool for nodes
    static constexpr size_t POOL_SIZE = 1024;
    alignas(64) std::array<Node, POOL_SIZE> node_pool;
    alignas(64) std::atomic<size_t> pool_index{0};

    Node* allocate_node(T value) {
        size_t index = pool_index.fetch_add(1, std::memory_order_relaxed) % POOL_SIZE;
        return new (&node_pool[index]) Node(std::move(value));
    }

public:
    LockFreeQueue() {
        Node* dummy = allocate_node(T());
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }
    
    ~LockFreeQueue() {
        Node* current = head.load(std::memory_order_acquire);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            current->~Node();
            current = next;
        }
    }

    void enqueue(T value) {
        Node* newNode = allocate_node(std::move(value));
        while (true) {
            Node* oldTail = tail.load(std::memory_order_acquire);
            Node* next = oldTail->next.load(std::memory_order_acquire);
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
            Node* oldHead = head.load(std::memory_order_acquire);
            Node* next = oldHead->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return false;
            }
            if (head.compare_exchange_weak(oldHead, next,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                result = std::move(next->data);
                oldHead->~Node();
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

class alignas(64) ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false), next_queue(0) {
        queues.reserve(numThreads);
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
        
        size_t index = next_queue.fetch_add(1, std::memory_order_relaxed) % queues.size();
        queues[index]->enqueue([task]() { (*task)(); });
        
        cv.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::atomic_store(&stop, true);
        }
        cv.notify_all();

        for (std::thread& worker : workers) {
            worker.join();
        }
    }

private:
    void workerThread(size_t id) {
        std::mt19937 rng(id);  // Random number generator for work stealing
        std::uniform_int_distribution<size_t> dist(0, queues.size() - 1);

        while (true) {
            std::function<void()> task;
            bool gotTask = queues[id]->dequeue(task);
            
            if (!gotTask) {
                // Work stealing
                for (size_t i = 0; i < queues.size() - 1; ++i) {
                    size_t victim = dist(rng);
                    if (victim != id && queues[victim]->steal(task)) {
                        gotTask = true;
                        break;
                    }
                }
            }
            
            if (gotTask) {
                task();
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { 
                    return std::atomic_load(&stop) || std::any_of(queues.begin(), queues.end(), 
                               [](const auto& q) { return !q->isEmpty(); });
                });
                
                if (std::atomic_load(&stop) && std::all_of(queues.begin(), queues.end(), 
                    [](const auto& q) { return q->isEmpty(); })) {
                    return;
                }
            }
        }
    }

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop;
    alignas(64) std::atomic<size_t> next_queue;
};

#endif // THREAD_POOL_H
