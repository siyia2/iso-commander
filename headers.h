#ifndef HEADERS_H
#define HEADERS_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <execution>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mntent.h>
#include <mutex>
#include <queue>
#include <readline/readline.h>
#include <readline/history.h>
#include <set>
#include <string>
#include <sys/mount.h>
#include <thread>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>


// Global and shared classes and variables


// Get max available CPU cores for global use
extern unsigned int maxThreads;

// Mutexes for global use
extern std::mutex Mutex4Low;
extern std::mutex Mutex4High;

// For making cache refresh headless
extern bool promptFlag;

// A simple global thread pool for async tasks
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


// SANITIZATION&EXTRACTION&READLINE

//voids

void loadHistory();
void saveHistory();

//stds

std::string shell_escape(const std::string& s);
std::pair<std::string, std::string> extractDirectoryAndFilename(const std::string& path);
std::string readInputLine(const std::string& prompt);

bool isValidLinuxPathFormat(const std::string& path);

//	CP&MV&RM

//	bools

// General
bool isValidLinuxPathFormat(const std::string& path);

// RM functions

//	bools
bool fileExists(const std::string& filename);

//	voids

// General
void select_and_operate_files_by_number(const std::string& operation);
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& operationSet, const std::string& process);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete);

// RM functions
void handleDeleteIsoFile(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& deletedSet);


//	MOUNTER ELITE

//	bools

// Mount functions
bool isAlreadyMounted(const std::string& mountPoint);

// Iso cache functions
bool ends_with_iso(const std::string& str);
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);

// Unmount functions
bool isDirectoryEmpty(const std::string& path);
bool isValidIndex(int index, size_t isoDirsSize);

//	voids

//General functions
bool isAllZeros(const std::string& str);
bool isNumeric(const std::string& str);

// Mount functions
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::unordered_set<std::string>& mountedSet);
void select_iso_files_by_number(const std::string& action);
void printIsoFileList(const std::vector<std::string>& isoFiles);
void processIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet, const std::vector<std::string>& isoDirs, const std::string& action);

// Iso cache functions
void manualRefreshCache(const std::string& initialDir = "");
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& Mutex4Low);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void removeNonExistentPathsFromCache();

// Unmount functions
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::vector<std::string>& isoDirs);

// Art
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1();
void submenu2();
void print_ascii();

//	stds

// Cache functions
std::future<bool> iequals(std::string_view a, std::string_view b);
std::future<bool> FileExists(const std::string& path);
std::string getHomeDirectory();
std::vector<std::string> loadCache();


// CONVERSION TOOLS

// General

// bools
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf);

// stds
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback);

// voids
void select_and_convert_files_to_iso(const std::string& fileTypeChoice);
void processInput(const std::string& input, const std::vector<std::string>& fileList, const std::string& inputPaths, bool flag);
void printFileList(const std::vector<std::string>& fileList);

// bools
bool fileExistsConversions(const std::string& fullPath);


// BIN/IMG CONVERSION

// bools
bool isCcd2IsoInstalled();


// voids
void convertBINToISO(const std::string& inputPath);


// MDF/MDS CONVERSION

// bools
bool isMdf2IsoInstalled();


// voids
void convertMDFToISO(const std::string& inputPath);

#endif // HEADERS_H
