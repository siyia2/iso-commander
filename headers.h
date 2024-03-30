#ifndef HEADERS_H
#define HEADERS_H

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <execution>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mntent.h>
#include <mutex>
#include <queue>
#include <readline/readline.h>
#include <readline/history.h>
#include <semaphore.h>
#include <set>
#include <string>
#include <sys/mount.h>
#include <thread>
#include <unordered_set>
#include <vector>


// Global and shared classes and variables

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


// SANITIZATION&EXTRACTION&READLINE

//voids

void loadHistory();
void saveHistory();

//stds

std::string shell_escape(const std::string& s);
std::pair<std::string, std::string> extractDirectoryAndFilename(const std::string& path);
std::string readInputLine(const std::string& prompt);


//	MOUNTER ELITE

//	bools

//Delete functions
bool fileExists(const std::string& filename);

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

//Delete functions
void select_and_delete_files_by_number();
void handleDeleteIsoFile(const std::string& iso, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);
void processDeleteInput(const char* input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);
void handleIsoFiles(const std::vector<std::string>& isos, std::unordered_set<std::string>& mountedSet);

// Mount functions
void mountIsoFile(const std::string& isoFile, std::unordered_set<std::string>& mountedSet);
void select_and_mount_files_by_number();
void printIsoFileList(const std::vector<std::string>& isoFiles);
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet);

// Iso cache functions
void manualRefreshCache();
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& Mutex4Low);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void removeNonExistentPathsFromCache();

// Unmount functions
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::string& isoDir);

// Art
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1();
void submenu2();
void print_ascii();

//	stds

// Cache functions
std::vector<std::string> vec_concat(const std::vector<std::string>& v1, const std::vector<std::string>& v2);
std::future<bool> iequals(std::string_view a, std::string_view b);
std::future<bool> FileExists(const std::string& path);
std::string getHomeDirectory();
std::vector<std::string> loadCache();


// CONVERSION TOOLS

// General

// bools

bool fileExistsConversions(const std::string& fullPath);


// BIN/IMG CONVERSION

// bools

bool blacklistBin(const std::filesystem::path& entry);

// stds
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);

// voids

void convertBINToISO(const std::string& inputPath);
void select_and_convert_files_to_iso();
void processInputBin(const std::string& input, const std::vector<std::string>& fileList);
bool isCcd2IsoInstalled();
void printFileListBin(const std::vector<std::string>& fileList);

// MDF/MDS CONVERSION

// bools
bool blacklistMDF(const std::filesystem::path& entry);

// stds
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);

// voids

void processInputMDF(const std::string& input, const std::vector<std::string>& fileList);
void convertMDFToISO(const std::string& inputPath);
void select_and_convert_files_to_iso_mdf();
bool isMdf2IsoInstalled();
void printFileListMdf(const std::vector<std::string>& fileList);

#endif // HEADERS_H
