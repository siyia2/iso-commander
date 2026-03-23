// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"


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


void mountIsoFiles(
    const std::vector<std::string>& isoFiles,
    std::unordered_set<std::string>& mountedFiles,
    std::unordered_set<std::string>& skippedMessages,
    std::unordered_set<std::string>& mountedFails,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    // Each thread owns its own context — no shared mutable state
    struct libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            mountedFails.insert(
                "\033[1;91mFailed to create mount context.\033[0m"
            );
        }
        return;
    }
    // RAII guard so ctx is always freed, even on early return
    struct CtxGuard {
        libmnt_context* c;
        ~CtxGuard() { mnt_free_context(c); }
    } ctxGuard{ctx};

    // Cache is read-only after construction — safe to share or rebuild per thread
    const std::unordered_set<std::string> mountPointCache = buildMountPointCache();

    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    VerbosityFormatter formatter;

    if (!silentMode) {
        constexpr size_t BATCH_SIZE = 1000;
        tempMountedFiles.reserve(BATCH_SIZE);
        tempSkippedMessages.reserve(BATCH_SIZE);
        tempMountedFails.reserve(BATCH_SIZE);
    }

    auto flushBuffers = [&]() {
        // Guard is inside the lambda; caller never needs to check silentMode
        if (silentMode) return;
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(),    tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(),    tempMountedFails.end());
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
    };

    for (const auto& isoFile : isoFiles) {
        fs::path isoPath(isoFile);
        auto [isoDirectory, isoFilename] =
            extractDirectoryAndFilename(isoFile, "mount");

        if (g_operationCancelled.load()) {
            if (!silentMode)
                tempMountedFails.push_back(
                    formatter.formatError(isoDirectory, isoFilename, "cxl"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        if (geteuid() != 0) {
            if (!silentMode)
                tempMountedFails.push_back(
                    formatter.formatError(isoDirectory, isoFilename, "needsRoot"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Hash-based unique mount point (unchanged)
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
        auto [mountisoDirectory, mountisoFilename] =
            extractDirectoryAndFilename(mountPoint, "mount");

        if (mountPointCache.count(mountPoint)) {
            if (!silentMode)
                tempSkippedMessages.push_back(formatter.formatSkipped(
                    isoDirectory, isoFilename,
                    mountisoDirectory, mountisoFilename));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        if (!fs::exists(isoPath)) {
            if (!silentMode)
                tempMountedFails.push_back(
                    formatter.formatError(isoDirectory, isoFilename, "missingISO"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Create_directory returns false (not throw) when dir already
        // exists; only propagate genuine errors
        std::error_code ec;
        fs::create_directory(mountPoint, ec);
        if (ec && ec != std::errc::file_exists) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatDetailedError(
                    isoDirectory, isoFilename, "Failed to create mount directory"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

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
                tempMountedFails.push_back(
                    formatter.formatError(isoDirectory, isoFilename, "badFS"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            //Ignore remove() errors — directory may already be gone
            // or non-empty; either way there's nothing useful to do here
            fs::remove(mountPoint, ec);
        }

        if (!silentMode &&
            (tempMountedFiles.size()    >= 1000 ||
             tempSkippedMessages.size() >= 1000 ||
             tempMountedFails.size()    >= 1000))
        {
            flushBuffers();
        }
    }

    //Always safe to call — flushBuffers no-ops in silentMode
    flushBuffers();
}
