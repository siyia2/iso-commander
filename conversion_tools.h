// conversion_tools.h

#ifndef CONVERSION_TOOLS_H
#define CONVERSION_TOOLS_H

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <execution>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <queue>
#include <regex>
#include <semaphore.h>
#include <set>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unordered_set>
#include <vector>

// A simple global thread pool for async tasks
class ThreadPool {
public:
    ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i)
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty())
                            return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            // don't allow enqueueing after stopping the pool
            if (stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers)
            worker.join();
    }

private:
    // need to keep track of threads so we can join them
    std::vector<std::thread> workers;
    // the task queue
    std::queue<std::function<void()>> tasks;
    // synchronization
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

// Get max available CPU cores for global use, fallback is 2 cores
extern unsigned int maxThreads;


// Function prototypes

// General
bool fileExistsConversions(const std::string& fullPath);
std::string toLower(const std::string& str);
bool endsWith(const std::string& fullString, const std::string& ending);


// BIN/IMG CONVERSION
bool blacklistBin(const std::filesystem::path& entry);
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
void convertBINToISO(const std::string& inputPath);
void select_and_convert_files_to_iso();
void processInputBin(const std::string& input, const std::vector<std::string>& fileList);
bool isCcd2IsoInstalled();
void printFileListBin(const std::vector<std::string>& fileList);

// MDF/MDS CONVERSION
bool blacklistMDF(const std::filesystem::path& entry);
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
void processInputMDF(const std::string& input, const std::vector<std::string>& fileList);
void convertMDFToISO(const std::string& inputPath);
void select_and_convert_files_to_iso_mdf();
bool isMdf2IsoInstalled();
void printFileListMdf(const std::vector<std::string>& fileList);
#endif // CONVERSION_TOOLS_H
