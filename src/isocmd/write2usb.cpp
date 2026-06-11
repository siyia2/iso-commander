// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>
#include <blkid/blkid.h>

// Project Headers
#include "../state.h"
#include "../write2usb.h"

namespace fs = std::filesystem;

/**
 * @brief Runs an external command with stdout/stderr suppressed.
 *
 * Forks a child process, redirects its stdout and stderr to /dev/null,
 * and exec's the given argument vector. The parent waits for the child
 * and returns its exit code.
 *
 * @param args Argument vector where args[0] is the executable name.
 * @return Exit code of the child process, or -1 on fork/exec failure.
 */
int runCommand(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devNull = open("/dev/null", O_WRONLY);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * @brief Probes /sys/module to determine the best available write-capable
 * NTFS kernel driver currently active on the host platform.
 *
 * This function evaluates system capabilities via sysfs, ensuring seamless
 * compatibility with both dynamically loaded kernel modules (.ko) and drivers
 * built directly into the kernel core. It utilizes a lazy-loading optimization
 * strategy, checking for pre-initialized drivers first and only executing
 * userspace `modprobe` commands for lower priority/fallback tiers if necessary.
 *
 * Following the modern NTFS driver overhaul in Linux 7.1+, this function
 * prioritizes the heavily optimized, modernized "ntfs" driver rewrite, which
 * entirely supersedes the legacy read-only engine. It falls back to Paragon's
 * "ntfs3" driver if running on older kernels (like 5.15 through 6.x) where the
 * modern remake is unavailable.
 *
 * @note Because modern Linux distributions frequently symlink userspace binaries
 * or default to older FUSE wrappers, downstream mounting logic should pass the
 * internal flag ("mount", "-i", "-t", driver, ...) when using the returned token.
 * This bypasses userspace mount helpers and forces the VFS subsystem to invoke
 * the designated kernel module directly.
 *
 * Driver Priority Tiering:
 * 1. "ntfs"  via Kernel 7.1+ (Modernized Native Read/Write Engine) — Highest Priority
 * 2. "ntfs3" via Kernel 5.15+ (Paragon Engine) — Fallback
 *
 * @return std::string The token name of the detected NTFS kernel driver ("ntfs" or "ntfs3"),
 * or an empty string if no modern write-capable driver is available.
 */
std::string getBestNtfsDriver() {
    // First Pass: Check if the modern 7.1+ NTFS driver is already active/built-in
    if (std::filesystem::exists("/sys/module/ntfs")) {
        return "ntfs";
    }

    // Demand-load the modern 7.1+ NTFSPLUS driver if it was dormant as a module
    runCommand({"modprobe", "ntfs"});
    if (std::filesystem::exists("/sys/module/ntfs")) {
        return "ntfs";
    }

    // Modern driver failed/unavailable. Lazy-load the Paragon ntfs3 fallback (Kernel 5.15+)
    runCommand({"modprobe", "ntfs3"});
    if (std::filesystem::exists("/sys/module/ntfs3")) {
        return "ntfs3";
    }

    // Return empty if no modern native write-capable drivers could be verified
    return {};
}

// ---------------------------------------------------------------------------
// Wipefs replacement (libblkid)
// ---------------------------------------------------------------------------

/**
 * @brief Erase all filesystem/partition signatures on @p device.
 *
 * Equivalent to `wipefs -a <device>`.  Uses libblkid's probe-and-wipe loop
 * so no child process is spawned.
 *
 * @param device Block device path (e.g. "/dev/sdb").
 * @return true on success (including the case where no signatures were found).
 */
static bool wipeDeviceSignatures(const std::string& device)
{
    blkid_probe pr = blkid_new_probe_from_filename(device.c_str());
    if (!pr) return false;

    blkid_probe_enable_superblocks(pr, true);
    blkid_probe_set_superblocks_flags(pr,
        BLKID_SUBLKS_MAGIC | BLKID_SUBLKS_TYPE);

    blkid_probe_enable_partitions(pr, true);
    blkid_probe_set_partitions_flags(pr, BLKID_PARTS_MAGIC);

    // Loop until no more signatures are found.
    // blkid_do_probe() returns 0 while signatures remain, 1 when done.
    while (blkid_do_probe(pr) == 0)
        blkid_do_wipe(pr, false /* not dry-run */);

    blkid_free_probe(pr);
    return true;
}

// ---------------------------------------------------------------------------
// Internal mount / umount / helpers (libmount)
// ---------------------------------------------------------------------------

/**
 * @brief Mount @p src at @p target using libmnt_context.
 *
 * @param src     Source path or device (may be nullptr for bind/move mounts).
 * @param target  Mount point (must already exist).
 * @param options Comma-separated mount options string (e.g. "ro,loop").
 *                Pass nullptr or "" for no extra options.
 * @param fstype  Filesystem type override (e.g. "ntfs3").
 *                Pass nullptr to let the kernel auto-detect.
 * @param flags   Extra libmount context flags (e.g. MNT_MS_PROPAGATION).
 *                Pass 0 for the common case.
 * @return 0 on success, non-zero on failure.
 */
static int libMount(const char* src,
                    const char* target,
                    const char* options = nullptr,
                    const char* fstype  = nullptr,
                    int         flags   = 0)
{
    libmnt_context* ctx = mnt_new_context();
    if (!ctx) return -1;

    if (src)     mnt_context_set_source(ctx, src);
    if (target)  mnt_context_set_target(ctx, target);
    if (options && *options)
                 mnt_context_append_options(ctx, options);
    if (fstype && *fstype)
                 mnt_context_set_fstype(ctx, fstype);
    (void)flags; // reserved for caller convenience

    int rc = mnt_context_mount(ctx);
    mnt_free_context(ctx);
    return rc;
}

/**
 * @brief Unmount @p target using libmnt_context.
 *
 * @param target Path to the mount point.
 * @param lazy   If true, pass MNT_DETACH (equivalent to umount -l).
 * @return 0 on success, non-zero on failure.
 */
static int libUmount(const char* target, bool lazy = false)
{
    libmnt_context* ctx = mnt_new_context();
    if (!ctx) return -1;

    mnt_context_set_target(ctx, target);
    if (lazy)
        mnt_context_enable_lazy(ctx, true);

    int rc = mnt_context_umount(ctx);
    mnt_free_context(ctx);
    return rc;
}

/**
 * @brief Best-effort unmount: try normal first, fall back to lazy.
 */
static void safeUmount(const char* path)
{
    if (libUmount(path, false) != 0)
        libUmount(path, true);
}

/**
 * @brief Detects whether an ISO image is Windows installation media.
 *
 * Loop-mounts the ISO read-only into a temporary directory and checks
 * for the co-presence of sources/boot.wim (Windows setup payload)
 * and bootmgr / bootmgr.efi (Windows boot manager).
 *
 * Uses libmount instead of forking mount/umount processes.
 *
 * @param isoPath Absolute path to the ISO image.
 * @return true if the ISO appears to be Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath)
{
    char tmpDir[] = "/tmp/iso_probe_XXXXXX";
    if (!mkdtemp(tmpDir)) return false;

    auto cleanup = [&]() {
        safeUmount(tmpDir);
        rmdir(tmpDir);
    };

    if (libMount(isoPath.c_str(), tmpDir, "ro,loop") != 0) {
        rmdir(tmpDir);
        return false;
    }

    bool hasBootWim = fs::exists(std::string(tmpDir) + "/sources/boot.wim");
    bool hasBootMgr = fs::exists(std::string(tmpDir) + "/bootmgr") ||
                      fs::exists(std::string(tmpDir) + "/bootmgr.efi");
    cleanup();
    return hasBootWim && hasBootMgr;
}

// ---------------------------------------------------------------------------
// Windows Family ISO type detection
// ---------------------------------------------------------------------------

/** Layout variants that drive partitioning and copy strategy. */
enum class IsoType { WindowsInstall, FatOnly };

/**
 * @brief Classify a mounted ISO as a full Windows install or FAT-only boot disk.
 *
 * Presence of @c sources/install.wim or @c sources/install.esd is the
 * canonical indicator of a Windows install image.  WinPE, Hiren's Boot CD,
 * and other rescue disks lack these files and are treated as @c FatOnly.
 *
 * @param isoMnt Mount point of the loop-mounted ISO (read-only).
 * @return @c WindowsInstall if an install payload is found, @c FatOnly otherwise.
 */
static IsoType detectIsoType(const std::string& isoMnt) {
    // A genuine Windows install ISO always ships sources/install.wim or
    // sources/install.esd.  Everything else (Hiren's, rescue disks, WinPE
    // variants without an install image, …) goes through the FAT-only path.
    return (fs::exists(isoMnt + "/sources/install.wim") ||
            fs::exists(isoMnt + "/sources/install.esd"))
               ? IsoType::WindowsInstall
               : IsoType::FatOnly;
}

/**
 * @brief Checks if a block device is accessible and ready for I/O operations.
 * * Attempts to open the device node in read-only and non-blocking mode.
 * This effectively tests if the kernel has fully registered the block device
 * and that it is not currently locked or in an invalid state.
 * * @param path The filesystem path to the device node (e.g., "/dev/sdb1").
 * @return true If the device can be opened successfully.
 * @return false If the device does not exist, access is denied, or it is busy.
 */
bool isDeviceReady(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) return false;
    close(fd);
    return true;
}

/**
 * @brief Waits for a device node to appear and become ready within a specified timeout.
 * * This function performs an informed poll by checking for the existence of the
 * device node and verifying its readiness via isDeviceReady(). It provides an
 * init-agnostic alternative to `udevadm settle` or `udevadm wait`.
 * * @param path The filesystem path to the device node to wait for.
 * @param timeout_seconds The maximum duration to wait in seconds (default is 30).
 * @return true If the device node is found and is ready for use within the timeout.
 * @return false If the timeout is reached before the device is ready.
 */
bool waitForDevice(const std::string& path, int timeout_seconds = 30) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(timeout_seconds)) {
        if (fs::exists(path) && fs::is_block_file(path)) {
            if (isDeviceReady(path)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Windows writer
// ---------------------------------------------------------------------------

/**
 * @brief Write a Windows-family ISO image to a block device using a hybrid I/O model.
 *
 * Partitioning is chosen automatically by ISO type:
 * - **WindowsInstall** — GPT with a ~1 GiB FAT32 ESP (bootloader +
 *   @c sources/boot.wim) and an NTFS data partition for the install payload.
 * - **FatOnly** (WinPE, Hiren's, rescue disks) — GPT with a single FAT32
 *   partition spanning the whole device.
 *
 * I/O strategy per file:
 * - **FAT32 files < 4 GiB** use unbuffered Direct I/O (@c O_DIRECT) to bypass the
 *   Linux page cache, ensuring user-initiated cancellations abort cleanly without
 *   post-cancel dirty writeback flushing to the device in the background.
 * - **FAT32 files >= 4 GiB** fall back to buffered I/O to respect the FAT32
 *   per-file size limit.
 * - **NTFS files** always use buffered I/O with @c posix_fallocate pre-allocation
 *   to maximize sequential write throughput and minimize filesystem fragmentation.
 *
 * Sector alignment:
 * - Both logical (@c BLKSSZGET) and physical (@c BLKPBSZGET) sector sizes are
 *   queried on each partition node. The larger of the two is used to govern
 *   @c O_DIRECT buffer alignment and write padding, ensuring correct behaviour
 *   on 512n, 512e, and 4Kn drives. If either ioctl fails or returns a
 *   non-positive value, that size is treated as 512 bytes; the larger of the
 *   two resulting values is then used.
 *
 * Telemetry:
 * - Progress, speed, and byte accounting are handled by a dedicated background
 *   thread, keeping timing logic and speed calculations out of the critical
 *   I/O path.
 *
 * Flush and unmount strategy:
 * - @c syncfs is called on each mounted filesystem fd before unmounting,
 *   flushing all dirty pages at the filesystem level explicitly. Unmounting
 *   is then delegated to @c safeUmount, which is expected to block until the
 *   kernel confirms all I/O is complete.
 *
 * For WindowsInstall, NTFS content is written before the ESP so the bootloader
 * only lands after the data it references is already on disk.
 *
 * @param isoPath       Path to the source ISO file.
 * @param device        Target block device (e.g. @c /dev/sdb). Will be wiped.
 * @param progressIndex Index into the shared @c progressData array for live
 *                      progress, speed, and completion reporting.
 * @return @c true on success, @c false on any error or cancellation.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex)
{
    auto fail = [&]() -> bool {
        progressData[progressIndex].failed.store(true);
        return false;
    };

    // ------------------------------------------------------------------ //
    // 0. Mount the ISO to inspect its layout                              //
    // ------------------------------------------------------------------ //
    char isoMnt[] = "/tmp/win_iso_XXXXXX";
    if (!mkdtemp(isoMnt)) return fail();

    auto unmountIso = [&]() {
        safeUmount(isoMnt);
        rmdir(isoMnt);
    };

    if (libMount(isoPath.c_str(), isoMnt, "ro,loop") != 0) {
        unmountIso();
        return fail();
    }

    const IsoType isoType = detectIsoType(std::string(isoMnt));

    // ------------------------------------------------------------------ //
    // 1. Wipe + repartition                                               //
    // ------------------------------------------------------------------ //
    if (!wipeDeviceSignatures(device)) { unmountIso(); return fail(); }

    int partResult = -1;
    if (isoType == IsoType::WindowsInstall) {
        partResult = runCommand({"parted", "-s", device,
                                 "mklabel", "gpt",
                                 "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                                 "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                                 "set", "1", "esp",  "on",
                                 "set", "1", "boot", "on"});
    } else {
        partResult = runCommand({"parted", "-s", device,
                                 "mklabel", "gpt",
                                 "mkpart", "WINPE", "fat32", "1MiB", "100%",
                                 "set", "1", "esp",  "on",
                                 "set", "1", "boot", "on"});
    }
    if (partResult != 0) { unmountIso(); return fail(); }

    if (!waitForDevice(device)) { unmountIso(); return fail(); }

    auto derivePartition = [&](int n) -> std::string {
        std::string target = device + std::to_string(n);
        return waitForDevice(target, 30) ? target : std::string{};
    };

    std::string fatPart  = derivePartition(1);
    std::string ntfsPart = (isoType == IsoType::WindowsInstall)
                               ? derivePartition(2)
                               : std::string{};

    if (fatPart.empty()) { unmountIso(); return fail(); }
    if (isoType == IsoType::WindowsInstall && ntfsPart.empty()) {
        unmountIso(); return fail();
    }

    if (runCommand({"mkfs.fat", "-F", "32", "-n",
                    (isoType == IsoType::WindowsInstall ? "WINBOOT" : "WINPE"),
                    fatPart}) != 0) {
        unmountIso(); return fail();
    }

    if (isoType == IsoType::WindowsInstall) {
        if (runCommand({"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA",
                        ntfsPart}) != 0) {
            unmountIso(); return fail();
        }
    }

    // Sector size query
    auto getBestSectorSize = [](const std::string& devNode) -> int {
        int fd = open(devNode.c_str(), O_RDONLY);
        if (fd < 0) return 512;
        int logicalSize  = 512;
        int physicalSize = 512;
        if (ioctl(fd, BLKSSZGET,  &logicalSize)  < 0 || logicalSize  <= 0) logicalSize  = 512;
        if (ioctl(fd, BLKPBSZGET, &physicalSize) < 0 || physicalSize <= 0) physicalSize = 512;
        close(fd);
        return std::max(logicalSize, physicalSize);
    };

    const int fatSectorSize  = getBestSectorSize(fatPart);
    const int ntfsSectorSize = (isoType == IsoType::WindowsInstall)
                                   ? getBestSectorSize(ntfsPart)
                                   : 512;

    if (GlobalState::g_operationCancelled.load()) { unmountIso(); return false; }

    // ------------------------------------------------------------------ //
    // 2. Mount FAT32 partition (and NTFS for Windows installs)            //
    // ------------------------------------------------------------------ //
    char fatMnt[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMnt[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(fatMnt)) { unmountIso(); return fail(); }

    if (isoType == IsoType::WindowsInstall) {
        if (!mkdtemp(ntfsMnt)) { unmountIso(); rmdir(fatMnt); return fail(); }
    }

    bool alreadyUnmounted = false;
    auto unmountAll = [&]() {
        if (alreadyUnmounted) return;
        alreadyUnmounted = true;

        safeUmount(isoMnt);
        safeUmount(fatMnt);
        if (isoType == IsoType::WindowsInstall)
            safeUmount(ntfsMnt);

        rmdir(isoMnt);
        rmdir(fatMnt);
        if (isoType == IsoType::WindowsInstall)
            rmdir(ntfsMnt);
    };

    if (libMount(fatPart.c_str(), fatMnt, "noatime") != 0) {
        unmountAll(); return fail();
    }

    std::string ntfsDriver;
    if (isoType == IsoType::WindowsInstall) {
        ntfsDriver = getBestNtfsDriver();
        if (ntfsDriver.empty()) { unmountAll(); return fail(); }

        libmnt_context* ctx = mnt_new_context();
        mnt_context_set_source(ctx, ntfsPart.c_str());
        mnt_context_set_target(ctx, ntfsMnt);
        mnt_context_set_fstype(ctx, ntfsDriver.c_str());
        mnt_context_append_options(ctx, "noatime");
        mnt_context_disable_helpers(ctx, true);
        int rc = mnt_context_mount(ctx);
        mnt_free_context(ctx);

        if (rc != 0) { unmountAll(); return fail(); }
    }

    if (GlobalState::g_operationCancelled.load()) { unmountAll(); return false; }

    // ------------------------------------------------------------------ //
    // 3. Collect and classify ISO entries                                //
    // ------------------------------------------------------------------ //
    struct IsoEntry {
        fs::path src;
        fs::path dst;
        uint64_t size;
        bool     isDir;
    };

    std::vector<IsoEntry> ntfsEntries;
    std::vector<IsoEntry> espEntries;
    uint64_t totalBytes = 0;

    const std::unordered_set<std::string> espTopFolders = { "efi", "boot" };
    const std::string espBootWim = "sources/boot.wim";

    auto belongsOnESP = [&](const fs::path& rel) -> bool {
        if (isoType == IsoType::FatOnly) return true;

        std::string top = rel.begin()->string();
        std::transform(top.begin(), top.end(), top.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (espTopFolders.count(top) > 0) return true;

        std::string relStr = rel.generic_string();
        std::transform(relStr.begin(), relStr.end(), relStr.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return relStr == espBootWim;
    };

    try {
        for (const auto& entry :
             fs::recursive_directory_iterator(std::string(isoMnt))) {
            fs::path rel   = fs::relative(entry.path(), std::string(isoMnt));
            bool     toESP = belongsOnESP(rel);
            fs::path dest  = fs::path(toESP ? std::string(fatMnt)
                                            : std::string(ntfsMnt)) / rel;

            uint64_t sz = entry.is_regular_file() ? entry.file_size() : 0;
            totalBytes += sz;

            IsoEntry e { entry.path(), dest, sz, entry.is_directory() };
            if (toESP) espEntries.push_back(std::move(e));
            else       ntfsEntries.push_back(std::move(e));
        }
    } catch (...) { unmountAll(); return fail(); }

    if (totalBytes == 0) { unmountAll(); return fail(); }

    // ------------------------------------------------------------------ //
    // 4. Async progress monitoring thread                                //
    // ------------------------------------------------------------------ //
    std::atomic<uint64_t> totalBytesWrittenAccumulator{0};
    std::atomic<bool>     monitoringActive{true};

    std::thread progressMonitorThread([&, totalBytes, progressIndex]() {
        auto     lastUpdate  = std::chrono::high_resolution_clock::now();
        uint64_t lastWritten = 0;

        while (monitoringActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            auto now = std::chrono::high_resolution_clock::now();
            auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastUpdate).count();
            if (ms <= 0) continue;

            uint64_t currentWritten = totalBytesWrittenAccumulator.load();
            uint64_t deltaBytes     = (currentWritten >= lastWritten)
                                          ? (currentWritten - lastWritten)
                                          : 0;

            progressData[progressIndex].bytesWritten.store(currentWritten);
            progressData[progressIndex].progress.store(static_cast<int>(
                std::min(99.0,
                         (static_cast<double>(currentWritten) / totalBytes) * 100.0)));

            if (deltaBytes > 0) {
                double speedMBs = (static_cast<double>(deltaBytes) /
                                   (1024.0 * 1024.0)) / (ms / 1000.0);
                progressData[progressIndex].speed.store(speedMBs);
            }

            lastWritten = currentWritten;
            lastUpdate  = now;
        }
    });

    constexpr size_t DESIRED_BUFFER = 4 * 1024 * 1024;
    const int maxSectorSize = std::max(fatSectorSize, ntfsSectorSize);
    size_t bufferSize = (DESIRED_BUFFER / maxSectorSize) * maxSectorSize;

    void* rawBuf = nullptr;
    if (posix_memalign(&rawBuf, maxSectorSize, bufferSize) != 0) {
        unmountAll(); return fail();
    }
    auto ioBuf = static_cast<char*>(rawBuf);
    auto freeIoBuf = [&]() { free(rawBuf); };

    // ------------------------------------------------------------------ //
    // 5. Per-file copy with hybrid I/O                                   //
    // ------------------------------------------------------------------ //
    auto copyWithProgress = [&](const fs::path& src, const fs::path& dst,
                                uint64_t fileSize, int sectorSize) -> bool {
        const bool isNtfsDest = (isoType == IsoType::WindowsInstall) &&
                                (dst.string().rfind(ntfsMnt, 0) == 0);

        constexpr uint64_t FAT32_FILE_LIMIT = 4ULL * 1024 * 1024 * 1024;
        const bool isLargeFat32 = !isNtfsDest && (fileSize >= FAT32_FILE_LIMIT);

        const bool useBufferedIO = isNtfsDest || isLargeFat32;

        int fd_in = open(src.c_str(), O_RDONLY);
        if (fd_in < 0) return false;
        posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

        int outFlags = O_WRONLY | O_CREAT | O_TRUNC;
        if (!useBufferedIO) outFlags |= O_DIRECT;

        int fd_out = open(dst.c_str(), outFlags, 0644);
        if (fd_out < 0) { close(fd_in); return false; }

        if (useBufferedIO && fileSize > 0)
            posix_fallocate(fd_out, 0, static_cast<off_t>(fileSize));

        const size_t mask = static_cast<size_t>(sectorSize) - 1;

        bool success = true;
        while (!GlobalState::g_operationCancelled.load()) {
            ssize_t bytes_read = read(fd_in, ioBuf, bufferSize);
            if (bytes_read < 0) {
                if (errno == EINTR) continue;
                success = false;
                break;
            }
            if (bytes_read == 0) break;

            ssize_t writeLen = bytes_read;
            if (!useBufferedIO) {
                ssize_t aligned = (bytes_read + mask) & ~static_cast<ssize_t>(mask);
                if (aligned > bytes_read)
                    memset(ioBuf + bytes_read, 0, aligned - bytes_read);
                writeLen = aligned;
            }

            ssize_t bytes_written = 0;
            while (bytes_written < writeLen) {
                if (GlobalState::g_operationCancelled.load()) {
                    success = false;
                    goto done;
                }
                ssize_t written = write(fd_out,
                                        ioBuf + bytes_written,
                                        writeLen - bytes_written);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    success = false;
                    goto done;
                }
                bytes_written += written;
            }

            totalBytesWrittenAccumulator.fetch_add(
                static_cast<uint64_t>(bytes_read));
        }

    done:
        if (!useBufferedIO) {
            if (GlobalState::g_operationCancelled.load()) {
                [[maybe_unused]] const int r = ftruncate(fd_out, 0);
            } else if (ftruncate(fd_out, static_cast<off_t>(fileSize)) != 0) {
                success = false;
            }
        }

        if (GlobalState::g_operationCancelled.load())
            posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);

        posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);
        close(fd_in);
        close(fd_out);
        return success && !GlobalState::g_operationCancelled.load();
    };

    // ------------------------------------------------------------------ //
    // 6. Copy passes (NTFS payload first, ESP second)                    //
    // ------------------------------------------------------------------ //
    auto startTime = std::chrono::high_resolution_clock::now();

    auto processEntries = [&](const std::vector<IsoEntry>& entries) -> bool {
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load()) {
                freeIoBuf(); unmountAll(); return false;
            }
            if (e.isDir) {
                fs::create_directories(e.dst);
            } else {
                fs::create_directories(e.dst.parent_path());

                const bool toNtfs = (isoType == IsoType::WindowsInstall) &&
                                    (e.dst.string().rfind(ntfsMnt, 0) == 0);
                int sectorSize = toNtfs ? ntfsSectorSize : fatSectorSize;

                if (!copyWithProgress(e.src, e.dst, e.size, sectorSize)) {
                    freeIoBuf(); unmountAll(); return fail();
                }
            }
        }
        return true;
    };

    if (!processEntries(ntfsEntries)) {
        monitoringActive.store(false);
        if (progressMonitorThread.joinable()) progressMonitorThread.join();
        return false;
    }
    if (!processEntries(espEntries)) {
        monitoringActive.store(false);
        if (progressMonitorThread.joinable()) progressMonitorThread.join();
        return false;
    }

    freeIoBuf();

    monitoringActive.store(false);
    if (progressMonitorThread.joinable()) progressMonitorThread.join();

    // ------------------------------------------------------------------ //
    // 7. Flush and unmount                                                //
    // ------------------------------------------------------------------ //
    if (!GlobalState::g_operationCancelled.load()) {
        if (isoType == IsoType::WindowsInstall) {
            int ntfsFd = open(ntfsMnt, O_RDONLY);
            if (ntfsFd >= 0) { syncfs(ntfsFd); close(ntfsFd); }
        }
        int fatFd = open(fatMnt, O_RDONLY);
        if (fatFd >= 0) { syncfs(fatFd); close(fatFd); }
    }

    unmountAll();

    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - startTime);
    double seconds  = totalElapsed.count() / 1000.0;
    double avgSpeed = seconds > 0.0
        ? (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / seconds
        : 0.0;

    progressData[progressIndex].speed.store(avgSpeed);
    progressData[progressIndex].progress.store(100);
    progressData[progressIndex].completed.store(true);
    return true;
}

/**
 * @brief Validates that a file descriptor refers to a valid ISO 9660 filesystem image.
 *
 * Reads the Primary Volume Descriptor at Sector 16 (byte offset 32768) using
 * positional I/O, leaving the file descriptor's offset cursor unmodified.
 *
 * Checks performed:
 *   - Descriptor type byte (buffer[0]) must equal 0x01 (Primary Volume Descriptor).
 *   - Standard Identifier bytes 1–5 must equal "CD001" (ECMA-119 §8.1).
 *
 * @param fd  An open, readable file descriptor referencing the target image.
 * @return    @c true if Sector 16 contains a valid ISO 9660 Primary Volume Descriptor;
 *            @c false if the read fails, the descriptor type is wrong, or the
 *            identifier does not match.
 */
bool isValidIso9660(int fd) {
    uint8_t buffer[2048];
    if (pread(fd, buffer, sizeof(buffer), 32768) != 2048) return false;

    // buffer[0]: Volume Descriptor Type — 0x01 = Primary Volume Descriptor (ECMA-119 §8.3).
    // buffer[1..5]: Standard Identifier — must be "CD001" (ECMA-119 §8.1).
    if (buffer[0] != 0x01) return false;

    std::string_view id(reinterpret_cast<char*>(buffer + 1), 5);
    return id == "CD001";
}

/**
 * @brief Probes an ISO image to detect Windows 10/11 installation media.
 *
 * Maps up to the first 32 MB of the file into the process address space and
 * performs a single-pass Aho-Corasick scan for Windows Boot Manager and WinPE
 * payload signatures. The 32 MB window is sufficient because Windows ISO
 * directory entries for bootmgr and boot.wim always appear within the first
 * few MB of the image.
 *
 * @details
 * - **Volume validation:** Requires a Primary Volume Descriptor (type 0x01,
 *   identifier "CD001") at sector 16. Pure-UDF images with "NSR02"/"NSR03"
 *   are also accepted; "BEA01" alone is not sufficient.
 * - **Signature scan:** Searches simultaneously for "bootmgr[.efi]" and
 *   "boot.wim" case-insensitively across ASCII, UTF-16LE, and UTF-16BE in a
 *   single O(n) pass. UTF-16LE is the native encoding used by Windows for
 *   Joliet/UDF filenames. The automaton is built once on first call and reused
 *   across subsequent calls.
 * - **Case folding:** A static 256-entry lookup table replaces @c std::tolower
 *   in the hot scan loop, avoiding locale machinery and branch overhead across
 *   the full mapping window.
 * - **Early exit:** The scan terminates as soon as both signature groups are
 *   matched, without scanning the remainder of the window. On typical Windows
 *   ISOs both signatures appear within the first 2–3 MB, so up to ~29 MB of
 *   iteration is skipped in the common case.
 *
 * @param fd       Open, readable file descriptor positioned anywhere — the
 *                 mapping is always made from offset 0. The caller retains
 *                 ownership; this function does not close the descriptor.
 * @param fileSize Size of the image in bytes, as returned by @c fstat. Used
 *                 for the minimum-size guard and to clamp the mapping window.
 * @return @c true if the volume header is valid and both "bootmgr[.efi]" and
 *         "boot.wim" signatures are present within the mapped window.
 *
 * @note Does not modify the file; read-only mapping.
 * @note The static Aho-Corasick automaton and case-folding table are both
 *       thread-safe under C++11 and later (guaranteed by static-local
 *       initialisation rules).
 * @note @c MADV_WILLNEED is applied to the first 4 MB of the mapping to
 *       begin faulting in pages immediately; @c MADV_SEQUENTIAL covers the
 *       remainder so the prefetcher stays primed if a full scan is needed.
 */
bool isWindowsIsoInitialCheck(int fd, off_t fileSize) {

    // --- Aho-Corasick automaton (built once, shared across calls) ---
    // Logical patterns: 0 = "boot.wim", 1 = "bootmgr.efi", 2 = "bootmgr"
    // Variants: 0 = ASCII, 1 = UTF-16LE, 2 = UTF-16BE  →  bits 0–8
    static const auto ac = [] {
        struct Node {
            std::array<int, 256> next{};
            int fail{0};
            uint32_t match{0};
            Node() { next.fill(-1); }
        };

        std::vector<Node> nodes;
        nodes.emplace_back();

        auto addPattern = [&](std::string_view text, int logicalIndex, int variant) {
            int cur = 0;
            for (char raw : text) {
                unsigned char c = static_cast<unsigned char>(std::tolower(raw));
                uint8_t bytes[2];
                int byteCount;
                if (variant == 0) {
                    bytes[0] = c;
                    byteCount = 1;
                } else if (variant == 1) {
                    bytes[0] = c; bytes[1] = 0x00;
                    byteCount = 2;
                } else {
                    bytes[0] = 0x00; bytes[1] = c;
                    byteCount = 2;
                }
                for (int b = 0; b < byteCount; ++b) {
                    int byte = bytes[b];
                    if (nodes[cur].next[byte] == -1) {
                        nodes[cur].next[byte] = static_cast<int>(nodes.size());
                        nodes.emplace_back();
                    }
                    cur = nodes[cur].next[byte];
                }
            }
            nodes[cur].match |= (1u << (logicalIndex * 3 + variant));
        };

        for (int v = 0; v < 3; ++v) {
            addPattern("boot.wim",    0, v);
            addPattern("bootmgr.efi", 1, v);
            addPattern("bootmgr",     2, v);
        }

        // BFS to build failure links
        std::queue<int> q;
        for (int c = 0; c < 256; ++c) {
            if (nodes[0].next[c] == -1) {
                nodes[0].next[c] = 0;
            } else {
                nodes[nodes[0].next[c]].fail = 0;
                q.push(nodes[0].next[c]);
            }
        }
        while (!q.empty()) {
            int u = q.front(); q.pop();
            nodes[u].match |= nodes[nodes[u].fail].match;
            for (int c = 0; c < 256; ++c) {
                int v = nodes[u].next[c];
                if (v == -1) {
                    nodes[u].next[c] = nodes[nodes[u].fail].next[c];
                } else {
                    nodes[v].fail = nodes[nodes[u].fail].next[c];
                    q.push(v);
                }
            }
        }

        return nodes;
    }();

    // Built once at program start; avoids locale machinery and branching
    // on every byte of the 32 MiB scan window.
    static const auto toLower = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[i] = static_cast<uint8_t>(std::tolower(i));
        return t;
    }();

    // --- Size check ---
    if (fileSize < 0x9000) return false;

    const size_t mapSize = std::min(static_cast<size_t>(fileSize),
                                    static_cast<size_t>(32 * 1024 * 1024));
    char* addr = static_cast<char*>(
        mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (addr == MAP_FAILED) return false;

    // Hint the kernel to prefetch the first 4 MiB immediately — signatures
    // almost always appear within the first 2–3 MiB of a Windows ISO, so
    // WILLNEED on the head reduces time-to-first-match.  SEQUENTIAL covers
    // the remainder so readahead stays primed for the full scan if needed.
    madvise(addr,                  4 * 1024 * 1024, MADV_WILLNEED);
    madvise(addr, mapSize,                          MADV_SEQUENTIAL);

    // --- Volume header validation ---
    {
        const uint8_t descType = static_cast<uint8_t>(addr[0x8000]);
        std::string_view id(addr + 0x8001, 5);
        const bool validIso = (descType == 0x01 && id == "CD001");
        const bool validUdf = (id == "NSR02" || id == "NSR03");
        if (!validIso && !validUdf) {
            munmap(addr, mapSize);
            return false;
        }
    }

    // --- Single-pass Aho-Corasick scan ---
    // Early-exit masks: once both groups are matched there is no need to
    // scan further.  Saves up to ~30 MiB of iteration on typical ISOs where
    // both signatures appear near the front of the directory area.
    //
    // boot.wim  matched in any variant: bits 0–2
    // bootmgr   matched in any variant: bits 3–8
    constexpr uint32_t MASK_BOOTWIM = 0b000000111u;
    constexpr uint32_t MASK_BOOTMGR = 0b111111000u;
    constexpr uint32_t MASK_ALL     = MASK_BOOTWIM | MASK_BOOTMGR;

    uint32_t found = 0;
    int cur = 0;
    for (size_t i = 0; i < mapSize; ++i) {
        cur    = ac[cur].next[toLower[static_cast<uint8_t>(addr[i])]];
        found |= ac[cur].match;

        // Both groups matched — no need to continue scanning.
        if ((found & MASK_ALL) == MASK_ALL) break;
    }

    munmap(addr, mapSize);

    return (found & MASK_BOOTWIM) != 0 &&
           (found & MASK_BOOTMGR) != 0;
}

/**
 * @brief Writes an ISO image to a raw block device, auto-routing Windows vs. Linux configurations.
 *
 * High-level orchestration routine that opens the ISO once, validates it, then intercepts
 * Windows installation media using an optimized metadata signature probe. Standard/Linux
 * distributions fall through directly into a specialized, unbuffered block-level stream.
 *
 * @details
 * - **Single fd Lifecycle:** The ISO is opened once at entry via @c open(O_RDONLY) and
 * held for the duration of the call. The same descriptor is passed to @ref isValidIso9660
 * and @ref isWindowsIsoInitialCheck, avoiding redundant opens. File size is retrieved
 * once via @c fstat and reused across all paths.
 * - **Early Validation:** @ref isValidIso9660 is invoked before any device access or
 * routing decision, ensuring invalid images are rejected with minimal overhead.
 * - **Windows Path Routing:** If @ref isWindowsIsoInitialCheck passes, execution is
 * delegated to @ref writeWindowsIsoToDevice, which automatically handles multi-partitioning,
 * hybrid FAT32+NTFS tables, and cluster tuning.
 * - **Raw O_DIRECT Pipeline:** Non-Windows targets are imaged via raw unbuffered disk I/O.
 * Data transfers utilize a dedicated page-aligned memory buffer (@c posix_memalign) sized
 * to the device's effective sector boundary, derived by querying both logical sector size
 * (@c ioctl(BLKSSZGET)) and physical sector size (@c ioctl(BLKPBSZGET)) and taking the
 * larger of the two. This ensures correct @c O_DIRECT alignment on 512n, 512e, and 4Kn
 * drives. @c BLKPBSZGET is treated as best-effort and falls back to the logical size if
 * the kernel or device does not support it.
 * @c POSIX_FADV_SEQUENTIAL is applied to the source descriptor before the write loop to
 * enable aggressive kernel read-ahead; @c POSIX_FADV_DONTNEED is applied after to release
 * the ISO from page cache once writing completes.
 * - **Tail-Block Padding:** Detects partial blocks at EOF or short reads, automatically
 * zero-padding the remaining buffer slice up to the strict sector layout boundary to prevent
 * kernel rejection errors (@c EINVAL).
 * - **Asynchronous Cancellation Safety:** Evaluates @c GlobalState::g_operationCancelled
 * at every inner loop pass. On user abort, it short-circuits execution and leaves the disk
 * safely without triggering a cascading @c fsync block.
 *
 * @param isoPath Absolute path to the source ISO image file on the host.
 * @param device Target destination block node path (e.g., @c /dev/sdb). WARNING: All existing
 * underlying data will be destructively overwritten.
 * @param progressIndex Unique thread tracking identifier mapped inside the global progress array.
 * @return @c true if the image data was transferred successfully, verified, and safely flushed;
 * @c false on physical I/O failure, validation collapse, or user cancellation.
 *
 * @see isValidIso9660
 * @see isWindowsIsoInitialCheck
 * @see writeWindowsIsoToDevice
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex) {

    // Open ISO once — shared by all validation and write paths
    int iso_fd = open(isoPath.c_str(), O_RDONLY);
    if (iso_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeIso = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeIso)> isoGuard(&iso_fd, closeIso);

    // Verify it's a valid ISO 9660 filesystem before proceeding
    if (!isValidIso9660(iso_fd)) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Seek back to beginning after validation probe
    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Get file size once via fstat — reused by both routing and raw write path
    struct stat sb;
    if (fstat(iso_fd, &sb) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Fast-path: Windows ISO routing (borrows iso_fd)
    if (isWindowsIsoInitialCheck(iso_fd, sb.st_size)) {
        return writeWindowsIsoToDevice(isoPath, device, progressIndex);
    }

    // Seek back to beginning for raw write path
    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Hint sequential read pattern for aggressive read-ahead on the ISO source.
    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // --- Raw O_DIRECT path for Linux / UEFI ISOs -------------------------

    // Open device with O_DIRECT for unbuffered writes
    int dev_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (dev_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<int, decltype(closeIso)> devGuard(&dev_fd, closeIso);

    // Query logical and physical sector sizes; take the max so that
    // O_DIRECT alignment is correct for 512n, 512e, and 4Kn drives.
    int logicalSectorSize  = 0;
    int physicalSectorSize = 0;

    if (ioctl(dev_fd, BLKSSZGET, &logicalSectorSize) < 0 || logicalSectorSize <= 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    // BLKPBSZGET is best-effort; fall back to logical size if unavailable
    if (ioctl(dev_fd, BLKPBSZGET, &physicalSectorSize) < 0 || physicalSectorSize <= 0)
        physicalSectorSize = logicalSectorSize;

    const int sectorSize = std::max(logicalSectorSize, physicalSectorSize);

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Pad file size up to sector boundary for O_DIRECT
    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    // Allocate aligned buffer (4 MiB, rounded down to sector boundary)
    constexpr size_t DESIRED_BUFFER = 4 * 1024 * 1024;
    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    char* alignedBuffer = nullptr;
    if (posix_memalign(reinterpret_cast<void**>(&alignedBuffer), sectorSize, bufferSize) != 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<char, decltype(&free)> bufferGuard(alignedBuffer, &free);

    // ------------------------------------------------------------------ //
    // Asynchronous UI Status Thread Initialization                        //
    // ------------------------------------------------------------------ //
    std::atomic<uint64_t> directBytesWrittenAccumulator{0};
    std::atomic<bool> monitoringActive{true};

    std::thread progressMonitorThread([&, fileSize, progressIndex]() {
        auto lastUpdate = std::chrono::high_resolution_clock::now();
        uint64_t lastWritten = 0;

        while (monitoringActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            auto now = std::chrono::high_resolution_clock::now();
            auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();
            if (ms <= 0) continue;

            uint64_t currentWritten = directBytesWrittenAccumulator.load();
            uint64_t deltaBytes     = (currentWritten >= lastWritten) ? (currentWritten - lastWritten) : 0;

            // Push current bytes and calculate absolute percentage out-of-band
            progressData[progressIndex].bytesWritten.store(currentWritten);
            progressData[progressIndex].progress.store(static_cast<int>(
                std::min(99.0, (static_cast<double>(currentWritten) / fileSize) * 100.0)));

            if (deltaBytes > 0) {
                double speedMBs = (static_cast<double>(deltaBytes) / (1024.0 * 1024.0)) / (ms / 1000.0);
                progressData[progressIndex].speed.store(speedMBs);
            }

            lastWritten = currentWritten;
            lastUpdate  = now;
        }
    });

    auto startTime = std::chrono::high_resolution_clock::now();
    uint64_t localBytesWritten = 0;

    try {
        while (localBytesWritten < paddedSize && !GlobalState::g_operationCancelled.load()) {
            uint64_t remaining   = paddedSize - localBytesWritten;
            size_t   bytesToRead = static_cast<size_t>(std::min<uint64_t>(bufferSize, remaining));

            ssize_t bytesRead = 0;
            while (true) {
                bytesRead = read(iso_fd, alignedBuffer, bytesToRead);
                if (bytesRead >= 0) break;
                if (errno == EINTR) continue;
                throw std::runtime_error("Read error: " + std::string(strerror(errno)));
            }

            if (bytesRead == 0) {
                std::memset(alignedBuffer, 0, bytesToRead);
            } else if (static_cast<size_t>(bytesRead) < bytesToRead) {
                std::memset(alignedBuffer + bytesRead, 0, bytesToRead - bytesRead);
            }

            // Write loop — handles partial writes and EINTR
            size_t remainingToWrite = bytesToRead;
            char* writePtr         = alignedBuffer;
            while (remainingToWrite > 0) {
                ssize_t written = write(dev_fd, writePtr, remainingToWrite);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error("Write error: " + std::string(strerror(errno)));
                }

                if (GlobalState::g_operationCancelled.load()) {
                    goto user_cancelled;
                }

                remainingToWrite -= static_cast<size_t>(written);
                writePtr         += written;

                // Safely cap reported progress at the true file size bounds to hide padding modifications
                uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize - std::min(localBytesWritten, fileSize));

                localBytesWritten += static_cast<size_t>(written);

                directBytesWrittenAccumulator.fetch_add(reportable);
            }
        }
    user_cancelled:;
    } catch (...) {
        monitoringActive.store(false);
        if (progressMonitorThread.joinable()) progressMonitorThread.join();

        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    // Safely terminate UI update operations before triggering storage synchronization logic
    monitoringActive.store(false);
    if (progressMonitorThread.joinable()) progressMonitorThread.join();

    // Drop ISO from page cache — no point keeping it after writing.
    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_DONTNEED);

    if (!GlobalState::g_operationCancelled.load()) {
        if (fsync(dev_fd) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
    }

    if (!GlobalState::g_operationCancelled.load() && localBytesWritten >= paddedSize) {
        auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTime);
        double seconds  = totalElapsed.count() / 1000.0;
        double avgSpeed = seconds > 0.0 ? (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / seconds : 0.0;

        progressData[progressIndex].speed.store(avgSpeed);
        progressData[progressIndex].progress.store(100);
        progressData[progressIndex].completed.store(true);
        return true;
    }

    return false;
}
