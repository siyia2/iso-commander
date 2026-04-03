// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"


// Function to build a cache of currently mounted points
static std::unordered_set<std::string> buildMountPointCache() {
    std::unordered_set<std::string> mounted;
    struct libmnt_table* tb = mnt_new_table_from_file("/proc/mounts");
    if (!tb) return mounted;

    struct libmnt_iter* itr = mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs* fs = nullptr;

    while (mnt_table_next_fs(tb, itr, &fs) == 0) {
        if (const char* target = mnt_fs_get_target(fs))
            mounted.emplace(target);
    }

    mnt_free_iter(itr);
    mnt_unref_table(tb);
    return mounted;
}


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, 
std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode) {
    // Check root once
    const bool hasRoot = (geteuid() == 0);

    // Create mount context
    struct libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            mountedFails.insert("\033[1;91mFailed to create mount context.\033[0m");
        }
        return;
    }

    // RAII guard ensures context is freed
    struct CtxGuard {
        libmnt_context* c;
        ~CtxGuard() { mnt_free_context(c); }
    } ctxGuard{ctx};

    // Cache already-mounted points
    const std::unordered_set<std::string> mountPointCache = buildMountPointCache();

    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    VerbosityFormatter formatter;

    if (!silentMode) {
        constexpr size_t BATCH_SIZE = 50;
        tempMountedFiles.reserve(BATCH_SIZE);
        tempSkippedMessages.reserve(BATCH_SIZE);
        tempMountedFails.reserve(BATCH_SIZE);
    }

    auto flushBuffers = [&]() {
        if (silentMode) return;
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
    };

    // Early exit if no root — mark all as failed
    if (!hasRoot) {
        for (const auto& isoFile : isoFiles) {
            if (!silentMode) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "needsRoot"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        flushBuffers();
        return;
    }

    for (const auto& isoFile : isoFiles) {
        fs::path isoPath(isoFile);
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");

        // Hash-based unique mount point
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        constexpr std::string_view base36 = "0123456789abcdefghijklmnopqrstuvwxyz";
        char shortHash[6] = {0};
        for (int i = 0; i < 5; ++i) {
            shortHash[i] = base36[hashValue % 36];
            hashValue /= 36;
        }

        const std::string uniqueId   = isoPath.stem().string() + "~" + shortHash;
        const std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        // Check if already mounted
        if (mountPointCache.count(mountPoint)) {
            if (!silentMode)
                tempSkippedMessages.push_back(formatter.formatSkipped(
                    isoDirectory, isoFilename,
                    mountisoDirectory, mountisoFilename));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Cancellation check **after already-mounted** — preserves successes
        if (g_operationCancelled.load()) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "cxl"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Check ISO exists
        if (!fs::exists(isoPath)) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "missingISO"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Create mount directory
        std::error_code ec;
        fs::create_directory(mountPoint, ec);
        if (ec && ec != std::errc::file_exists) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatDetailedError(
                    isoDirectory, isoFilename, "Failed to create mount directory"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Configure and mount
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype(ctx, "iso9660,udf,hfsplus,rockridge,joliet,isofs");

        const int ret = mnt_context_mount(ctx);
        if (ret == 0) {
            if (!silentMode) {
                std::string detectedFsType;
                if (const char* fstype = mnt_context_get_fstype(ctx))
                    detectedFsType = fstype;
                tempMountedFiles.push_back(formatter.formatMountSuccess(
                    isoDirectory, isoFilename,
                    mountisoDirectory, mountisoFilename,
                    detectedFsType));
            }
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "badFS"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            fs::remove(mountPoint, ec); // ignore errors
        }

        // Flush batches if needed
        if (!silentMode &&
            (tempMountedFiles.size() >= 50 ||
             tempSkippedMessages.size() >= 50 ||
             tempMountedFails.size() >= 50)) {
            flushBuffers();
        }
    }

    // Final flush
    flushBuffers();
}
