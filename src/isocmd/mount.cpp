// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"

/**
 * @brief Provides default LSan suppression rules for known third-party leaks.
 *
 * Suppresses false-positive leak reports originating from @c libmount's
 * internal @c realpath() allocation inside @c mnt_context_prepare_mount(),
 * which is never freed by the library before LSan's end-of-process scan.
 * The leak is confined entirely to @c libmount.so and is not reachable or
 * fixable from user code.
 *
 * This function is a weak symbol recognised by LeakSanitizer at startup;
 * it has no effect in non-ASan builds.
 *
 * @return A newline-separated list of LSan suppression patterns.
 */
extern "C" const char* __lsan_default_suppressions() {
    return "leak:mnt_context_prepare_mount\n";
}

/**
 * @brief Checks if a file is a valid disk image (ISO 9660 or UDF).
 *
 * Checks in order: ISO 9660, UDF (sector sizes 512/2048/4096).
 *
 * @param path Filesystem path to the file to be checked.
 * @return true if a recognised disk image signature is found, false otherwise.
 *
 * @note Performs binary reads only; does not modify the file.
 * @note UDF detection probes sector sizes 512, 2048, and 4096 for broad media support.
 *
 * @warning May return true for truncated images that still contain valid magic bytes.
 *
 * @see ISO 9660 ECMA-119, UDF OSTA standard.
 */
static bool isValidIsoFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    const auto fileSize = static_cast<std::streamoff>(f.tellg());

    // ISO 9660 PVD sits at byte 32769; UDF VRS with 512-byte sectors needs
    // at least sector 16 * 512 + 6 = 8198 bytes. 34816 comfortably covers both.
    if (fileSize < 34816) return false;

    // Seek to offset, read exactly n bytes. Returns false on short read.
    auto readAt = [&](std::streamoff offset, char* buf, std::streamsize n) -> bool {
        if (offset + n > fileSize) return false;
        f.clear();
        return f.seekg(offset) && f.read(buf, n) && f.gcount() == n;
    };

    char sig[5];

    // --- 1. ISO 9660 (CD001) at sector 16, byte offset +1 ---
    if (readAt(32769, sig, 5) && std::string_view(sig, 5) == "CD001")
        return true;

    // --- 2. UDF Volume Recognition Sequence ---
    static constexpr int kUdfSectors[]  = {16, 17, 18, 19, 256};
    static constexpr int kSectorSizes[] = {512, 2048, 4096};

    for (const int secSize : kSectorSizes) {
        for (const int sector : kUdfSectors) {
            if (!readAt(static_cast<std::streamoff>(sector) * secSize + 1, sig, 5))
                continue;
            std::string_view sv(sig, 5);
            if (sv == "BEA01" || sv == "NSR02" || sv == "NSR03" || sv == "TEA01")
                return true;
        }
    }

    return false;
}

/**
 * @brief Scans /proc/mounts and returns the set of currently active mount targets.
 */
static std::unordered_set<std::string> buildMountPointCache() {
    std::unordered_set<std::string> mounted;
    struct libmnt_table* tb = mnt_new_table_from_file("/proc/mounts");
    if (!tb) return mounted;

    struct libmnt_iter* itr = mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs*   fs  = nullptr;

    while (mnt_table_next_fs(tb, itr, &fs) == 0) {
        if (const char* target = mnt_fs_get_target(fs))
            mounted.emplace(target);
    }

    mnt_free_iter(itr);
    mnt_unref_table(tb);
    return mounted;
}

/**
 * @brief Computes a deterministic 5-character base-36 mount point suffix from a path.
 *
 * Uses FNV-1a for a stable, platform-independent hash (unlike std::hash).
 */
static std::string mountPointSuffix(const std::string& isoPath) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : isoPath) {
        h ^= c;
        h *= 1099511628211ULL;
    }

    static constexpr std::string_view kBase36 =
        "0123456789abcdefghijklmnopqrstuvwxyz";

    char out[6] = {};
    for (int i = 0; i < 5; ++i) {
        out[i] = kBase36[h % 36];
        h /= 36;
    }
    return out;
}

/**
 * @brief Mounts valid ISO files to unique directories under /mnt using loop devices.
 *
 * Performs progressive validation (root → cancellation → existence → format) before
 * attempting mount, minimising unnecessary I/O. A single libmnt_context is allocated
 * once for the batch and reset via mnt_reset_context between files to avoid repeated
 * allocation overhead. The in-memory mount point cache is updated on each successful
 * mount so duplicate paths within the same batch are caught without re-reading
 * /proc/mounts.
 *
 * Mount points: /mnt/iso_<stem>~<suffix> where suffix is derived from mountPointSuffix().
 *
 * @param isoFiles        Absolute paths to ISO files to process.
 * @param mountedFiles    Output: success messages (FS type included).
 * @param skippedMessages Output: already-mounted messages.
 * @param mountedFails    Output: failure messages (needsRoot, cxl, missingISO, badFS, mkdir).
 * @param completedTasks  Incremented for successes and skips.
 * @param failedTasks     Incremented for any failure (including cancellation).
 * @param silentMode      Suppresses all message generation; only counters are updated.
 *
 * @warning Requires root (geteuid() == 0). Without it every file gets "needsRoot".
 */
void mountIsoFiles(
    const std::vector<std::string>& isoFiles,
    std::unordered_set<std::string>& mountedFiles,
    std::unordered_set<std::string>& skippedMessages,
    std::unordered_set<std::string>& mountedFails,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    const bool hasRoot = (geteuid() == 0);

    // ----------------------------------------------------------------
    // Early-out: no root → everything fails fast, no libmount needed
    // ----------------------------------------------------------------
    if (!hasRoot) {
        if (!silentMode) {
            VerbosityFormatter formatter;
            std::vector<std::string> fails;
            fails.reserve(isoFiles.size());
            for (const auto& isoFile : isoFiles) {
                auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
                fails.push_back(formatter.formatError(dir, file, "needsRoot"));
            }
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            mountedFails.insert(fails.begin(), fails.end());
        }
        failedTasks->fetch_add(isoFiles.size(), std::memory_order_relaxed);
        return;
    }

    // ----------------------------------------------------------------
    // Allocate a single context for the whole batch (perf)
    // ----------------------------------------------------------------
    libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            mountedFails.insert("\033[1;91mFailed to create mount context.\033[0m");
        }
        return;
    }
    struct CtxGuard {
        libmnt_context* c;
        ~CtxGuard() { mnt_free_context(c); }
    } ctxGuard{ctx};

    // Mount point cache: pre-populated from /proc/mounts, updated in-loop
    // so duplicate paths within the same batch are detected immediately.
    auto mountPointCache = buildMountPointCache();  // non-const: must be updated on success

    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    VerbosityFormatter formatter;

    constexpr size_t kBatchSize = 50;
    if (!silentMode) {
        tempMountedFiles.reserve(kBatchSize);
        tempSkippedMessages.reserve(kBatchSize);
        tempMountedFails.reserve(kBatchSize);
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

    auto maybeFlush = [&]() {
        if (!silentMode &&
            (tempMountedFiles.size()    >= kBatchSize ||
             tempSkippedMessages.size() >= kBatchSize ||
             tempMountedFails.size()    >= kBatchSize)) {
            flushBuffers();
        }
    };

    auto recordFail = [&](const std::string& isoFile, const char* reason) {
        if (!silentMode) {
            auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
            tempMountedFails.push_back(formatter.formatError(dir, file, reason));
        }
        failedTasks->fetch_add(1, std::memory_order_relaxed);
    };

    for (const auto& isoFile : isoFiles) {

        if (g_operationCancelled.load(std::memory_order_relaxed)) {
            recordFail(isoFile, "cxl");
            maybeFlush();
            continue;
        }

        if (!fs::exists(fs::path(isoFile))) {
            recordFail(isoFile, "missingISO");
            maybeFlush();
            continue;
        }

        if (!isValidIsoFile(isoFile)) {
            recordFail(isoFile, "badFS");
            maybeFlush();
            continue;
        }

        const fs::path isoPath(isoFile);
        const std::string mountPoint =
            "/mnt/iso_" + isoPath.stem().string() + "~" + mountPointSuffix(isoFile);

        auto [mntDir, mntName] = extractDirectoryAndFilename(mountPoint, "mount");
        auto [isoDir, isoName] = extractDirectoryAndFilename(isoFile,    "mount");

        if (mountPointCache.count(mountPoint)) {
            if (!silentMode)
                tempSkippedMessages.push_back(
                    formatter.formatSkipped(isoDir, isoName, mntDir, mntName));
            completedTasks->fetch_add(1, std::memory_order_relaxed);
            maybeFlush();
            continue;
        }

        std::error_code ec;
        fs::create_directory(mountPoint, ec);

        if ((ec && ec != std::errc::file_exists) ||
            (fs::exists(mountPoint) && !fs::is_directory(mountPoint))) {
            recordFail(isoFile, "mkdir failed");
            maybeFlush();
            continue;
        }

        // Reset and reuse the shared context (perf)
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype_pattern(ctx, "iso9660,udf");

        const int ret = mnt_context_mount(ctx);

        if (ret == 0) {
            if (!silentMode) {
                const char* rawFsType = mnt_context_get_fstype(ctx);
                const std::string fsType = rawFsType ? rawFsType : "unknown";
                tempMountedFiles.push_back(
                    formatter.formatMountSuccess(isoDir, isoName, mntDir, mntName, fsType));
            }
            mountPointCache.emplace(mountPoint);
            completedTasks->fetch_add(1, std::memory_order_relaxed);
        } else {
            recordFail(isoFile, "badFS");
            fs::remove(mountPoint, ec);
        }

        maybeFlush();
    }

    flushBuffers();
}
