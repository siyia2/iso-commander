// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>

// Project Headers
#include "../concurrency.h"
#include "../mount.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../verbose.h"

struct libmnt_context;

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
 * Checks in order: ISO 9660 Primary Volume Descriptor, then UDF Volume
 * Recognition Sequence across sector sizes 2048, 512, and 4096 bytes.
 *
 * @param path Filesystem path to the file to be checked.
 * @return true if a recognised disk image signature is found, false otherwise.
 *
 * @note Performs binary reads only; does not modify the file.
 * @note ISO 9660 detection requires descriptor type 0x01 and identifier "CD001"
 *       at sector 16 (ECMA-119 §8.1).
 * @note UDF detection probes sectors {16, 17, 18, 19} across sector sizes 2048,
 *       512, and 4096 bytes. Sector 256 (Anchor Volume Descriptor Pointer) is
 *       excluded as it does not carry VRS identifiers. 2048-byte sectors are
 *       probed first as the common case.
 * @note Requires a minimum file size of 34816 bytes.
 *
 * @warning May return true for truncated images that still contain valid magic bytes.
 *
 * @see ECMA-119 (ISO 9660), ECMA-167 §7.2 (UDF VRS).
 */
static bool isValidIsoFile(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOATIME | O_CLOEXEC);
    // Note: O_NOATIME silently has no effect if caller doesn't own the file;
    // open() still succeeds, so this is safe for a read-only validator.
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 34816) {
        close(fd);
        return false;
    }
    const off_t fileSize = st.st_size;
    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

    // Buffer scoped inside lambda to avoid stale data across reads.
    char sig[6]{};
    auto readAt = [&](off_t offset, std::size_t n) -> bool {
        if (offset + static_cast<off_t>(n) > fileSize) return false;
        return pread(fd, sig, n, offset) == static_cast<ssize_t>(n);
    };

    bool found = false;

    // --- 1. ISO 9660: Primary Volume Descriptor at sector 16 ---
    // Byte 0 of the sector is the descriptor type; type 1 = Primary Volume Descriptor.
    // Identifier at bytes 1–5 must be "CD001" (ECMA-119 §8.1).
    if (readAt(32768, 6)) {
        if (sig[0] == 0x01 && std::string_view(sig + 1, 5) == "CD001")
            found = true;
    }

    // --- 2. UDF Volume Recognition Sequence ---
    // The VRS occupies sectors 16–19 (and optionally beyond) using 2048-byte sectors
    // per ECMA-167 §7.2. Sector sizes 512 and 4096 are probed for non-standard images.
    // 2048 is listed first as it is overwhelmingly the common case.
    //
    // Sector 256 (UDF Anchor VDP) is intentionally excluded: the tokens below
    // (BEA01, NSR0x, TEA01) are VRS identifiers and are not present at the anchor.
    if (!found) {
        static constexpr int kUdfSectors[]  = {16, 17, 18, 19};
        static constexpr int kSectorSizes[] = {2048, 512, 4096};

        for (const int secSize : kSectorSizes) {
            for (const int sector : kUdfSectors) {
                // VRS structure identifier begins at byte 1 of each sector (byte 0 is type).
                if (!readAt(static_cast<off_t>(sector) * secSize + 1, 5)) continue;
                std::string_view sv(sig, 5);
                if (sv == "BEA01" || sv == "NSR02" || sv == "NSR03" || sv == "TEA01") {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    return found;
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
 * Mount points: /mnt/iso_<stem>~<5-char base-36 FNV-1a suffix>.
 *
 * Results are written directly to the global verboseSets:
 *   - verboseSets.operationCompleted  Success messages (FS type included).
 *   - verboseSets.operationSkipped    Already-mounted messages.
 *   - verboseSets.operationFailed     Failure messages (needsRoot, cxl, missingISO, badFS, mkdir).
 *
 * @param isoFiles        Absolute paths to ISO files to process.
 * @param completedTasks  Incremented for successes and skips.
 * @param failedTasks     Incremented for any failure (including cancellation).
 * @param silentMode      Suppresses all message generation; only counters are updated.
 *
 * @warning Requires root (geteuid() == 0). Without it every file gets "needsRoot".
 */
void mountIsoFiles(
    const std::vector<std::string>& isoFiles,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    const bool hasRoot = (geteuid() == 0);

    if (!hasRoot) {
        if (!silentMode) {
            VerbosityFormatter formatter;
            std::vector<std::string> fails;
            fails.reserve(isoFiles.size());
            for (const auto& isoFile : isoFiles) {
                auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
                fails.push_back(formatter.formatError(std::string(dir), std::string(file), "needsRoot"));
            }
            std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
            verboseSets.operationFailed.insert(fails.begin(), fails.end());
        }
        failedTasks->fetch_add(isoFiles.size(), std::memory_order_relaxed);
        return;
    }

    libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
            verboseSets.operationFailed.insert("\033[1;91mFailed to create mount context.\033[0m");
        }
        return;
    }
    struct CtxGuard {
        libmnt_context* c;
        ~CtxGuard() { mnt_free_context(c); }
    } ctxGuard{ctx};

    auto mountPointCache = buildMountPointCache();

    std::vector<std::string> tempCompleted;
    std::vector<std::string> tempSkipped;
    std::vector<std::string> tempFailed;
    VerbosityFormatter formatter;

    constexpr size_t kBatchSize = 50;
    if (!silentMode) {
        tempCompleted.reserve(kBatchSize);
        tempSkipped.reserve(kBatchSize);
        tempFailed.reserve(kBatchSize);
    }

    auto flushBuffers = [&]() {
        if (silentMode) return;
        std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
        verboseSets.operationCompleted.insert(tempCompleted.begin(), tempCompleted.end());
        verboseSets.operationSkipped.insert(tempSkipped.begin(), tempSkipped.end());
        verboseSets.operationFailed.insert(tempFailed.begin(), tempFailed.end());
        tempCompleted.clear();
        tempSkipped.clear();
        tempFailed.clear();
    };

    auto maybeFlush = [&]() {
        if (!silentMode &&
            (tempCompleted.size() >= kBatchSize ||
             tempSkipped.size()   >= kBatchSize ||
             tempFailed.size()    >= kBatchSize)) {
            flushBuffers();
        }
    };

    auto recordFail = [&](const std::string& isoFile, const char* reason) {
        if (!silentMode) {
            auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
            tempFailed.push_back(formatter.formatError(std::string(dir), std::string(file), reason));
        }
        failedTasks->fetch_add(1, std::memory_order_relaxed);
    };

    for (const auto& isoFile : isoFiles) {

        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
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
                tempSkipped.push_back(
                    formatter.formatSkipped(std::string(isoDir), std::string(isoName), std::string(mntDir), std::string(mntName))
                );
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

        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype_pattern(ctx, "udf,iso9660");

        const int ret = mnt_context_mount(ctx);

        if (ret == 0) {
            if (!silentMode) {
                const char* rawFsType = mnt_context_get_fstype(ctx);
                const std::string fsType = rawFsType ? rawFsType : "unknown";
                tempCompleted.push_back(
                    formatter.formatMountSuccess(std::string(isoDir), std::string(isoName), std::string(mntDir), std::string(mntName), fsType)
                );
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
