// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"

/**
 * @brief Checks if a file is a valid disk image (ISO 9660, UDF, or HFS/HFS+/HFSX).
 *
 * Checks in order: ISO 9660, UDF (sector sizes 512/2048/4096), HFS/HFS+/HFSX.
 *
 * @param path Filesystem path to the file to be checked.
 * @return true if a recognised disk image signature is found, false otherwise.
 *
 * @note Performs binary reads only; does not modify the file.
 * @note A 300 KB minimum provides a practical lower bound covering ISO 9660 and
 *       HFS offsets. UDF coverage at large sector offsets is handled per-iteration.
 * @note UDF detection probes sector sizes 512, 2048, and 4096 for broad media support.
 * @note HFS magic numbers are big-endian; byte-swapped only on little-endian hosts.
 *
 * @warning May return true for truncated images that still contain valid magic bytes.
 *          Additional validation may be needed for production use.
 *
 * @see ISO 9660 ECMA-119, UDF OSTA standard, Apple HFS+ Technical Note TN1150.
 */
static bool isValidIsoFile(const std::string& path) {
    // Open with binary mode. We use ate to get size, but immediately check if open.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return false;
    }

    const auto fileSize = static_cast<std::streamoff>(f.tellg());
    
    // Minimum viable size for any of these formats (~34KB)
    // ISO9660 starts at 32KB + 2KB for the Primary Volume Descriptor.
    if (fileSize < 34816) {
        return false;
    }

    // Stack buffer for signatures to avoid allocations
    std::array<char, 5> sig;

    // --- 1. ISO 9660 (CD001) ---
    // Sector 16 (16 * 2048 = 32768). Identifier is at offset +1.
    f.clear();
    if (f.seekg(32769) && f.read(sig.data(), 5)) {
        if (std::string_view(sig.data(), 5) == "CD001") return true;
    }

    // --- 2. UDF (Universal Disk Format) ---
    // We check common Volume Recognition Sequence (VRS) locations.
    static constexpr int kUdfSectors[]  = {16, 17, 18, 19, 256};
    static constexpr int kSectorSizes[] = {512, 2048, 4096};

    for (const int secSize : kSectorSizes) {
        for (const int sector : kUdfSectors) {
            const auto offset = static_cast<std::streamoff>(sector) * secSize + 1;
            
            if (offset + 5 > fileSize) continue;

            f.clear();
            if (f.seekg(offset) && f.read(sig.data(), 5)) {
                std::string_view sv(sig.data(), 5);
                if (sv == "BEA01" || sv == "NSR02" || sv == "NSR03" || sv == "TEA01") {
                    return true;
                }
            }
        }
    }

    // --- 3. HFS / HFS+ / HFSX ---
	if (fileSize >= 1026) {
		f.clear();
		if (f.seekg(1024)) {
			unsigned char buf[2]; 
			if (f.read(reinterpret_cast<char*>(buf), 2) && f.gcount() == 2) {
				// Manually reconstruct: (High Byte << 8) | Low Byte
				// HFS is Big-Endian, so buf[0] is the most significant byte.
				uint16_t hfsMagic = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];

				if (hfsMagic == 0x4244 || // 'BD'
					hfsMagic == 0x482B || // 'H+'
					hfsMagic == 0x4858)   // 'HX'
				{
					return true;
				}
			}
		}
	}

    return false;
}

/**
 * @brief Scans /proc/mounts to identify currently active mount points.
 * * This helper uses libmount to read the kernel's mount table and provides
 * a quick lookup set to avoid redundant mount attempts.
 * * @return std::unordered_set<std::string> A set containing the target paths of all current mounts.
 */
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

/**
 * @brief Mounts valid ISO files to unique directories under /mnt using loop devices.
 * 
 * This function validates, mounts, and tracks ISO images with thread-safe batch reporting.
 * It performs progressive validation (root check → cancellation → existence → format check)
 * before attempting mount, minimizing unnecessary I/O on cancelled or invalid operations.
 * 
 * Mount points are created as /mnt/iso_<basename>~<5-char base-36 hash> where the hash
 * is derived from the full ISO path to ensure uniqueness. The function checks an
 * in-memory mount point cache to skip already-mounted files without repeating syscalls.
 * 
 * @param isoFiles        Vector of absolute paths to ISO files to process
 * @param mountedFiles    Output set for successful mount messages (format: success with detected FS type)
 * @param skippedMessages Output set for already-mounted files (format: source + destination paths)
 * @param mountedFails    Output set for failure messages (keys: needsRoot, cxl, missingISO, 
 *                        badFS, or detailed directory creation errors)
 * @param completedTasks  Atomic counter incremented for successes and skipped files
 * @param failedTasks     Atomic counter incremented for any failure (including cancellation)
 * @param silentMode      If true, suppresses all message generation and global set updates;
 *                        only atomic counters are updated. No temporary buffers are allocated.
 * 
 * @note The function performs format validation via isValidIsoFile() (magic bytes check)
 *       before mounting. A failure after validation (badFS) indicates a genuine mount error
 *       (kernel module, corruption, permissions) rather than format mismatch.
 * 
 * @note Mount uses libmount with fstype probe order: iso9660 → udf → hfsplus.
 *       Options set: "loop,ro" (read-only loop device).
 * 
 * @note Batches output updates every 50 entries per category to cap mutex contention
 *       and memory usage. Empty mount directories are cleaned up on mount failure.
 * 
 * @warning Requires root privileges (geteuid() == 0) to mount. Without root, all files
 *          report "needsRoot" failure without attempting any operations.
 * 
 * @warning Cancellation check (g_operationCancelled) occurs before existence/validation I/O,
 *          allowing prompt abort even on large file lists.
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

    const auto mountPointCache = buildMountPointCache();

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

    // ----------------------------------------------------------------
    // Early-out: no root → everything fails fast
    // ----------------------------------------------------------------
    if (!hasRoot) {
        for (const auto& isoFile : isoFiles) {
            if (!silentMode) {
                auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
                tempMountedFails.push_back(formatter.formatError(dir, file, "needsRoot"));
            }
            failedTasks->fetch_add(1, std::memory_order_relaxed);
        }
        flushBuffers();
        return;
    }

    for (const auto& isoFile : isoFiles) {

        // ------------------------------------------------------------
        // Cancellation (zero-cost check)
        // ------------------------------------------------------------
        if (g_operationCancelled.load(std::memory_order_relaxed)) {
            if (!silentMode) {
                auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
                tempMountedFails.push_back(formatter.formatError(dir, file, "cxl"));
            }
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        fs::path isoPath(isoFile);
        auto [isoDir, isoName] = extractDirectoryAndFilename(isoFile, "mount");

        // ------------------------------------------------------------
        // Existence check (cheap)
        // ------------------------------------------------------------
        if (!fs::exists(isoPath)) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatError(isoDir, isoName, "missingISO"));
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ------------------------------------------------------------
        // Format validation (expensive → after existence)
        // ------------------------------------------------------------
        if (!isValidIsoFile(isoFile)) {
            if (!silentMode)
                tempMountedFails.push_back(formatter.formatError(isoDir, isoName, "badFS"));
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ------------------------------------------------------------
        // Deterministic mountpoint
        // ------------------------------------------------------------
        const size_t hashValue = std::hash<std::string>{}(isoFile);

        constexpr std::string_view base36 = "0123456789abcdefghijklmnopqrstuvwxyz";
        char shortHash[6] = {};

        size_t tmp = hashValue;
        for (int i = 0; i < 5; ++i) {
            shortHash[i] = base36[tmp % 36];
            tmp /= 36;
        }

        const std::string mountPoint =
            "/mnt/iso_" + isoPath.stem().string() + "~" + shortHash;

        auto [mntDir, mntName] = extractDirectoryAndFilename(mountPoint, "mount");

        // ------------------------------------------------------------
        // Already mounted check
        // ------------------------------------------------------------
        if (mountPointCache.count(mountPoint)) {
            if (!silentMode)
                tempSkippedMessages.push_back(
                    formatter.formatSkipped(isoDir, isoName, mntDir, mntName));
            completedTasks->fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ------------------------------------------------------------
        // Create mountpoint
        // ------------------------------------------------------------
        std::error_code ec;
        fs::create_directory(mountPoint, ec);

        if (ec && ec != std::errc::file_exists) {
            if (!silentMode)
                tempMountedFails.push_back(
                    formatter.formatDetailedError(isoDir, isoName, "mkdir failed"));
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ------------------------------------------------------------
        // Mount via libmount (correct probing API)
        // ------------------------------------------------------------
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype_pattern(ctx, "iso9660,udf,hfsplus");

        const int ret = mnt_context_mount(ctx);

        if (ret == 0) {
            if (!silentMode) {
                std::string fsType;
                if (const char* t = mnt_context_get_fstype(ctx))
                    fsType = t;

                tempMountedFiles.push_back(
                    formatter.formatMountSuccess(isoDir, isoName, mntDir, mntName, fsType));
            }
            completedTasks->fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!silentMode)
                tempMountedFails.push_back(
                    formatter.formatError(isoDir, isoName, "badFS"));

            failedTasks->fetch_add(1, std::memory_order_relaxed);
            fs::remove(mountPoint, ec); // cleanup
        }

        // ------------------------------------------------------------
        // Batch flush
        // ------------------------------------------------------------
        if (!silentMode &&
            (tempMountedFiles.size() >= 50 ||
             tempSkippedMessages.size() >= 50 ||
             tempMountedFails.size() >= 50)) {
            flushBuffers();
        }
    }

    flushBuffers();
}
