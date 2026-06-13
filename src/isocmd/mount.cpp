// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>

// Project Headers
#include "../globalMutexes.h"
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
 * @brief Heuristic check to determine if a file is a valid ISO 9660 or UDF disk image.
 *
 * Performs a series of signature-based checks to identify disk image formats.
 * Validation is heuristic: it confirms the presence of standard volume descriptors
 * but does not perform a full filesystem integrity parse.
 *
 * @param path Filesystem path to the file to be checked.
 * @param st   The stat struct of the file (used for an initial size gate).
 * @return true if a recognized disk image signature is found, false otherwise.
 *
 * @note Performs binary reads only; does not modify the file.
 * @note ISO 9660 detection: Checks for descriptor type 0x01 and identifier "CD001"
 * at logical sector 16 (offset 32768, ECMA-119 §8.1).
 * @note UDF detection: Probes for Volume Recognition Sequence (VRS) identifiers
 * ("BEA01", "NSR02", "NSR03", "TEA01") at sectors 16-19.
 * - Checks sector sizes: 2048 (default), 512, and 4096 bytes.
 * - Identifier is read starting at byte 1 of each sector (byte 0 is type).
 * - Anchor Volume Descriptor Pointer (AVDP) at sector 256 is not checked.
 * @note Requires a minimum file size of 34816 bytes (17 * 2048).
 *
 * @warning May return true for truncated images that contain valid magic bytes.
 * @warning This is a header-level validator; a "true" result does not guarantee
 * that the subsequent filesystem structures are readable or intact.
 *
 * @see ECMA-119 (ISO 9660), ECMA-167 §7.2 (UDF VRS).
 */
static bool isValidIsoFile(const std::string& path, const struct stat& st) {
    // Early size check using pre-existing stat
    if (st.st_size < 34816) {
        return false;
    }
    const off_t fileSize = st.st_size;

    const int fd = open(path.c_str(), O_RDONLY | O_NOATIME | O_CLOEXEC);
    // Note: O_NOATIME silently has no effect if caller doesn't own the file;
    // open() still succeeds, so this is safe for a read-only validator.
    if (fd < 0) return false;

    // RAII guard for file descriptor
    struct FdGuard {
        int fd;
        ~FdGuard() { if (fd >= 0) close(fd); }
    } fdGuard{fd};

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
    return found;
    // fdGuard automatically closes the file descriptor
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
 * @brief Mounts a batch of ISO files to unique directories under /mnt using loop devices.
 *
 * This function handles bulk mounting by performing multi-stage validation (root privileges,
 * cancellation state, file existence, and filesystem format) to minimize unnecessary I/O.
 *
 * ### Integrity & Performance Features:
 * - **Resource Efficiency:** Uses a single `libmnt_context` per batch, reset via `mnt_reset_context`
 * to avoid repeated allocation overhead.
 * - **Intelligent Deduplication:**
 * 1. **Path-based:** Filters via `mountPointCache` for quick lookups.
 * 2. **Inode-based:** Inspects `/sys/block/loopN/loop/backing_file` to detect files that have
 * been renamed or hard-linked but are already active, ensuring idempotency.
 * - **Concurrency Strategy:** Results are buffered in thread-local vectors and flushed to
 * `globalSets` periodically to minimize lock contention on `GlobalMutexes::globalSetsMutex`.
 *
 * ### Mount Path Schema:
 * `/mnt/iso_<stem>~<5-char base-36 FNV-1a suffix>`
 *
 * @param isoFiles      Vector of absolute paths to the ISO files.
 * @param completedTasks Atomic counter for successful mounts and skipped duplicates.
 * @param failedTasks   Atomic counter for any failures (e.g., missing root, I/O errors).
 * @param silentMode    If true, suppresses all log generation; only updates atomics.
 *
 * @warning Requires root privileges (geteuid() == 0). If invoked without root, all
 * operations will immediately fail with a "needsRoot" error.
 * @note The inode cache is updated dynamically during the loop, allowing the function to
 * properly handle batches containing duplicate files.
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
            std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
            verboseSets.operationFailed.insert(fails.begin(), fails.end());
        }
        failedTasks->fetch_add(isoFiles.size(), std::memory_order_relaxed);
        return;
    }

    libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
            verboseSets.operationFailed.insert("\033[1;91mFailed to create mount context.\033[0m");
        }
        return;
    }
    struct CtxGuard {
        libmnt_context* c;
        ~CtxGuard() { mnt_free_context(c); }
    } ctxGuard{ctx};

    // Pack (st_dev, st_ino) into a single uint64_t key so the cache is
    // correct across multiple filesystems (inodes are only unique per device).
    auto makeInodeKey = [](const struct stat& st) -> uint64_t {
        return (static_cast<uint64_t>(st.st_dev) << 32) | st.st_ino;
    };

    // Build cache of already-mounted source inodes so that a renamed ISO
    // (same inode, different path) is correctly recognised as already mounted.
    // mnt_fs_get_source() returns the loop device (e.g. /dev/loop0) for ISO
    // mounts, not the backing file path, so we resolve it through sysfs.
    auto buildMountedInodeCache = [&makeInodeKey]() -> std::unordered_set<uint64_t> {
        std::unordered_set<uint64_t> inodes;
        libmnt_table* tbl = mnt_new_table_from_file("/proc/self/mountinfo");
        if (!tbl) return inodes;

        // RAII wrapper for libmount table
        struct TableGuard {
            libmnt_table* t;
            ~TableGuard() { if (t) mnt_free_table(t); }
        } tableGuard{tbl};

        libmnt_iter* itr = mnt_new_iter(MNT_ITER_FORWARD);
        if (!itr) return inodes;

        // RAII wrapper for libmount iterator
        struct IterGuard {
            libmnt_iter* i;
            ~IterGuard() { if (i) mnt_free_iter(i); }
        } iterGuard{itr};

        libmnt_fs* fs = nullptr;
        while (mnt_table_next_fs(tbl, itr, &fs) == 0) {
            const char* src = mnt_fs_get_source(fs);
            if (!src) continue;

            struct stat st{};
            if (::stat(src, &st) != 0) continue;

            if (S_ISBLK(st.st_mode)) {
                // Block device — check if it is a loop device backed by a file.
                // /sys/block/loopN/loop/backing_file contains the real path,
                // and stat() on that path gives the stable inode regardless of
                // any renames that have happened since the mount.
                unsigned int loopNum = 0;
                if (std::sscanf(src, "/dev/loop%u", &loopNum) == 1) {
                    // Use pre-sized string buffer to avoid multiple allocations
                    char backingPath[256];
                    int written = snprintf(backingPath, sizeof(backingPath),
                                          "/sys/block/loop%u/loop/backing_file", loopNum);
                    if (written <= 0 || static_cast<size_t>(written) >= sizeof(backingPath)) {
                        continue; // Path too long, skip this entry
                    }

                    // Use RAII for file stream
                    std::ifstream f(backingPath);
                    if (!f.is_open()) continue;

                    std::string realPath;
                    if (std::getline(f, realPath) && !realPath.empty()) {
                        // Trim trailing whitespace
                        while (!realPath.empty() &&
                               (realPath.back() == '\n' || realPath.back() == '\r'))
                            realPath.pop_back();

                        struct stat backingSt{};
                        if (::stat(realPath.c_str(), &backingSt) == 0)
                            inodes.insert(makeInodeKey(backingSt));
                    }
                    // f is automatically closed when it goes out of scope
                }
            } else {
                // Direct file mount (no loop device) — stat it normally.
                inodes.insert(makeInodeKey(st));
            }
        }
        // IterGuard and TableGuard automatically clean up
        return inodes;
    };

    auto mountPointCache = buildMountPointCache();
    auto mountedInodes   = buildMountedInodeCache();

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
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
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

    // Pre-allocate string buffer for mount point construction
    std::string mountPointBuffer;
    mountPointBuffer.reserve(256); // Typical path length

    for (const auto& isoFile : isoFiles) {
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
            recordFail(isoFile, "cxl");
            maybeFlush();
            continue;
        }

        // Single stat() call for existence check, validation, and inode cache
        struct stat isoStat{};
        if (::stat(isoFile.c_str(), &isoStat) != 0) {
            recordFail(isoFile, "missingISO");
            maybeFlush();
            continue;
        }

        // Check if it's a regular file
        if (!S_ISREG(isoStat.st_mode)) {
            recordFail(isoFile, "badFS");
            maybeFlush();
            continue;
        }

        // Validate ISO format using pre-existing stat result (avoids redundant stat)
        if (!isValidIsoFile(isoFile, isoStat)) {
            recordFail(isoFile, "badFS");
            maybeFlush();
            continue;
        }

        // Construct mount point with minimal allocations
        const fs::path isoPath(isoFile);
        mountPointBuffer.clear();
        mountPointBuffer = "/mnt/iso_";
        mountPointBuffer += isoPath.stem().string();
        mountPointBuffer += "~";
        mountPointBuffer += mountPointSuffix(isoFile);
        const std::string& mountPoint = mountPointBuffer;

        auto [mntDir, mntName] = extractDirectoryAndFilename(mountPoint, "mount");
        auto [isoDir, isoName] = extractDirectoryAndFilename(isoFile, "mount");

        // --- path-based skip (file was not renamed) ---
        if (mountPointCache.count(mountPoint)) {
            if (!silentMode)
                tempSkipped.push_back(
                    formatter.formatSkipped(std::string(isoDir), std::string(isoName),
                                            std::string(mntDir), std::string(mntName))
                );
            completedTasks->fetch_add(1, std::memory_order_relaxed);
            maybeFlush();
            continue;
        }

        // --- inode-based skip (file was renamed after being mounted) ---
        // The kernel tracks loop-device backing files by inode, so a rename
        // leaves the original mount intact but under a stale path in our
        // name-derived mountPointCache.  Detect this by checking whether the
        // file's (dev, inode) pair is already present in the live mount table.
        // Reuse the isoStat we already have
        if (mountedInodes.count(makeInodeKey(isoStat))) {
            if (!silentMode)
                tempSkipped.push_back(
                    formatter.formatSkipped(std::string(isoDir), std::string(isoName),
                                            std::string(mntDir), std::string(mntName))
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
                    formatter.formatMountSuccess(std::string(isoDir), std::string(isoName),
                                                 std::string(mntDir), std::string(mntName), fsType)
                );
            }
            mountPointCache.emplace(mountPoint);
            // Keep the inode cache consistent so subsequent entries in the
            // same batch that share the same file are also skipped correctly.
            mountedInodes.insert(makeInodeKey(isoStat));
            completedTasks->fetch_add(1, std::memory_order_relaxed);
        } else {
            recordFail(isoFile, "badFS");
            // Only remove the directory we just created if it is empty;
            // a concurrent mount may have legitimately used it.
            if (fs::is_directory(mountPoint) && fs::is_empty(mountPoint))
                fs::remove(mountPoint, ec);
        }

        maybeFlush();
    }

    flushBuffers();
}
