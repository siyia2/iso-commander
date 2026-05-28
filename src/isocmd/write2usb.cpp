// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
 * @brief Detects whether an ISO image is Windows installation media.
 *
 * Loop-mounts the ISO read-only into a temporary directory and checks
 * for the co-presence of @c sources/boot.wim (Windows setup payload)
 * and @c bootmgr / @c bootmgr.efi (Windows boot manager).  Both markers
 * must be present; either alone is insufficient.
 *
 * The temporary mount point is always unmounted and removed before the
 * function returns, regardless of outcome.
 *
 * @param isoPath Absolute path to the ISO image.
 * @return @c true if the ISO appears to be Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath) {
    char tmpDir[] = "/tmp/iso_probe_XXXXXX";
    if (!mkdtemp(tmpDir)) return false;

    auto cleanup = [&]() {
        std::string cmd = "umount -l " + std::string(tmpDir) + " 2>/dev/null";
        system(cmd.c_str());
        rmdir(tmpDir);
    };

    std::string mountCmd = "mount -o ro,loop " + isoPath + " " + tmpDir + " 2>/dev/null";
    if (system(mountCmd.c_str()) != 0) {
        rmdir(tmpDir);
        return false;
    }

    bool hasBootWim = fs::exists(std::string(tmpDir) + "/sources/boot.wim");
    bool hasBootMgr = fs::exists(std::string(tmpDir) + "/bootmgr") ||
                      fs::exists(std::string(tmpDir) + "/bootmgr.efi");
    cleanup();
    return hasBootWim && hasBootMgr;
}

/**
 * @brief Writes a Windows ISO to a block device using a hybrid FAT32 + NTFS layout (UEFI only).
 *
 * Partition layout written to @p device:
 *   GPT partition table.
 *   Partition 1: FAT32 (1024 MiB, EFI System Partition) — receives:
 *     - efi/
 *     - boot/
 *     - sources/boot.wim
 *
 *   This satisfies Windows UEFI boot requirements while remaining within
 *   FAT32 limitations.
 *
 *   Partition 2: NTFS (remainder) — receives all remaining files, including
 *   large files such as sources/install.wim that exceed FAT32's 4 GiB limit.
 *   Formatted with 64 KiB clusters (@c -c 65536) for better sequential write
 *   performance.
 *
 * This layout mirrors the approach used by modern Windows installation media:
 * a small FAT32 ESP for firmware compatibility, and a larger NTFS partition
 * for bulk data storage.
 *
 * The NTFS partition is mounted using the kernel's native ntfs3 driver
 * (requires Linux kernel 5.15 or newer). Fallback to ntfs-3g FUSE is not
 * implemented; the function will fail if ntfs3 is unavailable.
 *
 * @par Partition probing
 * After partitioning, @c udevadm @c settle is used to block until the kernel
 * has finished processing all uevents and the partition device nodes are
 * guaranteed to exist. This replaces the previous @c partprobe + @c sleep
 * polling approach, which was both slower and subject to races on loaded
 * systems. A short retry loop is retained as a safety net for environments
 * without udevd (e.g. minimal containers).
 *
 * @par Progress reporting
 * Progress, speed, and completion state are written atomically to
 * @c progressData[progressIndex]. Updates are emitted at most every 300 ms
 * during file copy; the clock is sampled on every 4 MiB chunk. There is no
 * long blocking phase after formatting.
 *
 * @par Copy strategy
 * Files are copied using a buffered @c read / @c write loop with a 4 MiB buffer,
 * which is allocated once and reused across all files. This approach performs
 * better than @c copy_file_range() over USB mass storage because it provides
 * better backpressure to the flash controller. The source file descriptor uses
 * @c posix_fadvise(POSIX_FADV_SEQUENTIAL) to enable kernel read-ahead from the
 * loop-mounted ISO.
 *
 * @par Preallocation
 * Destination files on the NTFS partition are preallocated using
 * @c posix_fallocate() on a best-effort basis to reduce NTFS fragmentation.
 * Errors are ignored. Preallocation is skipped for FAT32 (which would fall
 * back to zero-filling the file, causing a redundant write pass).
 *
 * @par Flush strategy
 * After all files are copied, the device is flushed with @c fsync() before
 * unmounting.
 *
 * @par Cancellation
 * @c GlobalState::g_operationCancelled is checked before and during every
 * file copy. On cancellation:
 *   - Copying stops at the next chunk boundary
 *   - @c POSIX_FADV_DONTNEED is issued on both partition block devices to
 *     discard dirty page-cache pages, preventing a writeback storm when
 *     the lazy unmount hands the mount to the kernel for async teardown
 *   - All three mounts are lazily unmounted (@c umount -l)
 *   - The function returns @c false without setting the failure flag
 *
 * @par Two-pass copy order
 * NTFS partition files are written first, followed by ESP (FAT32) files.
 * This ensures that the bootloader only lands on the ESP after all the data
 * it references (especially sources/boot.wim) is already present on the drive.
 * Windows setup is sensitive to the ESP appearing complete before the
 * referenced data is available.
 *
 * @param isoPath
 *   Absolute path to the source Windows ISO.
 *
 * @param device
 *   Absolute path to the target block device (e.g. @c /dev/sdb).
 *   All existing data on this device will be destroyed.
 *
 * @param progressIndex
 *   Index into @ref progressData used for reporting progress, speed,
 *   and completion status.
 *
 * @return @c true   The ISO was successfully written; the device is bootable.
 * @return @c false  An error occurred or the operation was cancelled.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                                    const std::string& device,
                                    size_t             progressIndex) {
    auto fail = [&]() -> bool {
        progressData[progressIndex].failed.store(true);
        return false;
    };

    // ------------------------------------------------------------------ //
    // 1. Wipe + repartition: GPT, FAT32 ESP (~1 GiB) + NTFS (remainder) //
    // ------------------------------------------------------------------ //
    if (runCommand({"wipefs", "-a", device}) != 0) return fail();

    if (runCommand({"parted", "-s", device,
                    "mklabel", "gpt",
                    "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                    "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                    "set", "1", "esp",  "on",
                    "set", "1", "boot", "on"}) != 0) return fail();

    if (runCommand({"udevadm", "settle"}) != 0) return fail();

    auto derivePartition = [&](int n) -> std::string {
        std::string c1 = device + std::to_string(n);
        std::string c2 = device + "p" + std::to_string(n);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (fs::exists(c1)) return c1;
            if (fs::exists(c2)) return c2;
            sleep(1);
        }
        return {};
    };

    std::string fatPart  = derivePartition(1);
    std::string ntfsPart = derivePartition(2);
    if (fatPart.empty() || ntfsPart.empty()) return fail();

    if (runCommand({"mkfs.fat", "-F", "32", "-n", "WINBOOT", fatPart}) != 0)
        return fail();

    if (runCommand({"mkfs.ntfs", "-f", "-c", "65536", "-L", "WINDATA", ntfsPart}) != 0)
        return fail();

    if (GlobalState::g_operationCancelled.load()) return false;

    // ------------------------------------------------------------------ //
    // 2. Mount ISO, FAT32 partition, and NTFS partition                  //
    // ------------------------------------------------------------------ //
    char isoMnt[]  = "/tmp/win_iso_XXXXXX";
    char fatMnt[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMnt[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(isoMnt) || !mkdtemp(fatMnt) || !mkdtemp(ntfsMnt))
        return fail();

    auto dropPageCache = [](const std::string& blockDev) {
        int fd = open(blockDev.c_str(), O_RDONLY);
        if (fd >= 0) {
            posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            close(fd);
        }
    };

    auto unmountAll = [&]() {
        if (GlobalState::g_operationCancelled.load()) {
            dropPageCache(fatPart);
            dropPageCache(ntfsPart);
        }
        runCommand({"umount", "-l", isoMnt});
        runCommand({"umount", "-l", fatMnt});
        runCommand({"umount", "-l", ntfsMnt});
        rmdir(isoMnt);
        rmdir(fatMnt);
        rmdir(ntfsMnt);
    };

    if (runCommand({"mount", "-o", "ro,loop", isoPath, isoMnt}) != 0) {
        unmountAll(); return fail();
    }
    if (runCommand({"mount", fatPart, fatMnt}) != 0) {
        unmountAll(); return fail();
    }
    if (runCommand({"mount", "-t", "ntfs3", ntfsPart, ntfsMnt}) != 0) {
        unmountAll(); return fail();
    }

    if (GlobalState::g_operationCancelled.load()) { unmountAll(); return false; }

    // ------------------------------------------------------------------ //
    // 3. Collect all ISO entries in a single pass, compute total bytes,  //
    //    and classify each entry (ESP vs NTFS) up front.                 //                                           //
    // ------------------------------------------------------------------ //
    struct IsoEntry {
        fs::path src;
        fs::path dst;      // fully resolved destination path
        uint64_t size;
        bool     isDir;
    };

    // ESP files go first (boot-critical), NTFS files go second.
    // We keep two vectors and concatenate so NTFS data is fully written
    // before the bootloader lands on the ESP — safer for Windows boot.
    std::vector<IsoEntry> ntfsEntries;
    std::vector<IsoEntry> espEntries;
    uint64_t totalBytes = 0;

    const std::unordered_set<std::string> espTopFolders = { "efi", "boot" };
    const std::string espBootWim = "sources/boot.wim";

    auto belongsOnESP = [&](const fs::path& rel) -> bool {
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
        for (const auto& entry : fs::recursive_directory_iterator(std::string(isoMnt))) {
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
    // 4. Copy with live progress                                         //
    //                                                                    //
    //    Key USB-specific choices:                      //
    //    - copy_file_range removed: it optimises in-kernel page-cache    //
    //      copies but adds no benefit writing over USB mass storage, and //
    //      can obscure backpressure. Plain read/write is faster and more //
    //      predictable on flash controllers.                             //
    //    - Buffer allocated once (4 MiB) and reused across all files.    //
    //      4 MiB keeps the controller's internal buffer fed              //
    //      without over-committing.                                      //
    //    - posix_fallocate on NTFS only: pre-reserves clusters so the    //
    //      driver doesn't allocate on every write. Skipped for FAT32     //
    //      which has no fallocate support and would zero-fill instead.   //
    //    - posix_fadvise(SEQUENTIAL) on each source fd: the read side is //
    //      a loop-mounted ISO on the host — telling the kernel to        //
    //      prefetch aggressively is a free throughput win.               //
    // ------------------------------------------------------------------ //
    auto startTime  = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    constexpr int UPDATE_INTERVAL_MS = 300;

    auto updateProgress = [&](uint64_t justCopied) {
        progressData[progressIndex].bytesWritten.fetch_add(justCopied);
        bytesInWindow += justCopied;

        auto now = std::chrono::high_resolution_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - lastUpdate).count();
        if (ms >= UPDATE_INTERVAL_MS) {
            uint64_t written = progressData[progressIndex].bytesWritten.load();
            progressData[progressIndex].progress.store(
                static_cast<int>(
                    (static_cast<double>(written) / totalBytes) * 100));
            if (bytesInWindow > 0 && ms > 0) {
                progressData[progressIndex].speed.store(
                    (static_cast<double>(bytesInWindow) /
                     (1024.0 * 1024.0)) / (ms / 1000.0));
                bytesInWindow = 0;
            }
            lastUpdate = now;
        }
    };

    // Single reusable 4 MiB buffer — allocated once here.
    constexpr size_t BUF_SIZE = 4 * 1024 * 1024;
    std::vector<char> ioBuf(BUF_SIZE);

    auto copyWithProgress = [&](const fs::path& src, const fs::path& dst,
                                uint64_t fileSize) -> bool {
        int fd_in = open(src.c_str(), O_RDONLY);
        if (fd_in < 0) return false;

        // Hint to the kernel: we will read this file sequentially.
        // The ISO is on the host filesystem, so prefetching is cheap and
        // effective — the kernel will read ahead into page cache while we
        // write to the USB device.
        posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

        int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0) { close(fd_in); return false; }

        // posix_fallocate on NTFS (ntfs3) pre-reserves clusters, avoiding
        // on-the-fly allocation during writing — measurably faster on USB.
        // Skipped for FAT32: it doesn't support fallocate and would fall
        // back to writing zeros (a full redundant write pass per file).
        const bool isNtfs = dst.string().rfind(ntfsMnt, 0) == 0;
        if (isNtfs && fileSize > 0)
            posix_fallocate(fd_out, 0, static_cast<off_t>(fileSize));  // ignore return; best-effort

        bool success = true;

        while (!GlobalState::g_operationCancelled.load()) {
            ssize_t bytes_read = read(fd_in, ioBuf.data(), BUF_SIZE);
            if (bytes_read < 0) {
                if (errno == EINTR) continue;
                success = false;
                break;
            }
            if (bytes_read == 0) break;  // EOF

            ssize_t bytes_written = 0;
            while (bytes_written < bytes_read) {
                if (GlobalState::g_operationCancelled.load()) {
                    success = false;
                    goto done;
                }
                ssize_t written = write(fd_out,
                                        ioBuf.data() + bytes_written,
                                        bytes_read   - bytes_written);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    success = false;
                    goto done;
                }
                bytes_written += written;
            }
            updateProgress(static_cast<uint64_t>(bytes_read));
        }

    done:
        close(fd_in);
        close(fd_out);
        return success && !GlobalState::g_operationCancelled.load();
    };

    // ------------------------------------------------------------------ //
    // 5. Two-pass copy: NTFS data first, ESP bootloader second.          //
    //    Windows setup is sensitive to the ESP appearing complete; by    //
    //    writing all NTFS content first we ensure the bootloader only    //
    //    lands after the data it points to is already on the drive.      //
    // ------------------------------------------------------------------ //
    auto processEntries = [&](const std::vector<IsoEntry>& entries) -> bool {
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load()) {
                unmountAll(); return false;
            }
            if (e.isDir) {
                fs::create_directories(e.dst);
            } else {
                fs::create_directories(e.dst.parent_path());
                if (!copyWithProgress(e.src, e.dst, e.size)) {
                    unmountAll(); return fail();
                }
            }
        }
        return true;
    };

    if (!processEntries(ntfsEntries)) return false;
    if (!processEntries(espEntries))  return false;

    // ------------------------------------------------------------------ //
    // 6. Flush and unmount                                               //
    //                                                                    //
    //    Note: USB mass storage devices may acknowledge fsync/flush      //
    //    commands before their internal write cache is fully committed   //
    //    to flash. There is no reliable userspace workaround for this.   //
    //    The umount calls below force the kernel to flush all dirty      //
    //    pages; that is the strongest guarantee we can give.             //
    // ------------------------------------------------------------------ //
    int diskFd = open(device.c_str(), O_RDWR);
    if (diskFd >= 0) {
        fsync(diskFd);
        close(diskFd);
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
 * @brief Writes an ISO image to a block device, auto-detecting Windows media.
 *
 * Calls @ref isWindowsIso to probe the ISO before writing.  Windows ISOs
 * are handled by @ref writeWindowsIsoToDevice (FAT32 format + file copy +
 * optional WIM split); all other ISOs fall through to a direct raw
 * O_DIRECT block write.
 *
 * For the raw path: opens the ISO for reading and the target device with
 * @c O_WRONLY|O_DIRECT, then streams data in sector-aligned chunks (default
 * 8 MiB buffer, rounded down to a multiple of the device sector size). If
 * the ISO size is not a multiple of the sector size, the final chunk is
 * zero-padded to the sector boundary to satisfy O_DIRECT alignment
 * requirements. Progress, speed, and completion state are written atomically
 * to @c progressData[progressIndex].
 *
 * Speed is recalculated every 300 ms using a sliding byte window.
 * The write loop checks @c GlobalState::g_operationCancelled before each
 * iteration and exits cleanly without marking the task as failed when a
 * cancellation is detected. @c fsync is called on the device fd only if
 * the write was not cancelled, avoiding unnecessary I/O on a partial transfer.
 *
 * @param isoPath       Absolute path to the source ISO image.
 * @param device        Absolute path to the target block device (e.g. @c /dev/sdb).
 * @param progressIndex Index into @ref progressData for this task.
 * @return @c true if every byte of the ISO was written successfully and the
 *         operation was not cancelled; @c false otherwise.
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex) {
    // Probe for Windows installation media and delegate if detected
    if (isWindowsIso(isoPath))
        return writeWindowsIsoToDevice(isoPath, device, progressIndex);

    // --- Raw O_DIRECT path for Linux / UEFI ISOs -------------------------

    // Open ISO for reading
    int iso_fd = open(isoPath.c_str(), O_RDONLY);
    if (iso_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeIso = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeIso)> isoGuard(&iso_fd, closeIso);

    // Open device with O_DIRECT for unbuffered writes
    int dev_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (dev_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeDev = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeDev)> devGuard(&dev_fd, closeDev);

    // Get sector size (required for O_DIRECT alignment)
    int sectorSize = 0;
    if (ioctl(dev_fd, BLKSSZGET, &sectorSize) < 0 || sectorSize <= 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    uint64_t fileSize = 0;
    try {
        fileSize = std::filesystem::file_size(isoPath);
    } catch (const std::filesystem::filesystem_error&) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    if (fileSize == 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Pad file size up to sector boundary for O_DIRECT
    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    // Allocate aligned buffer (8 MiB, rounded down to sector boundary)
    constexpr size_t DESIRED_BUFFER = 8 * 1024 * 1024;
    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    char* alignedBuffer = nullptr;
    if (posix_memalign(reinterpret_cast<void**>(&alignedBuffer), sectorSize, bufferSize) != 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<char, decltype(&free)> bufferGuard(alignedBuffer, &free);

    // Progress / speed tracking
    auto startTime  = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    constexpr int UPDATE_INTERVAL_MS = 300;

    auto updateSpeed = [&](auto now) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
        if (bytesInWindow > 0 && elapsed.count() > 0) {
            double seconds = elapsed.count() / 1000.0;
            progressData[progressIndex].speed.store(
                (static_cast<double>(bytesInWindow) / (1024.0 * 1024.0)) / seconds);
            bytesInWindow = 0;
            lastUpdate = now;
        }
    };

    uint64_t localBytesWritten = 0;

    try {
        while (localBytesWritten < paddedSize && !GlobalState::g_operationCancelled.load()) {
            uint64_t remaining   = paddedSize - localBytesWritten;
            size_t   bytesToRead = static_cast<size_t>(std::min<uint64_t>(bufferSize, remaining));

            ssize_t bytesRead = 0;
            while (true) {
                bytesRead = read(iso_fd, alignedBuffer, bytesToRead);
                if (bytesRead >= 0) break;
                if (errno == EINTR)  continue;
                throw std::runtime_error("Read error: " + std::string(strerror(errno)));
            }

            if (bytesRead == 0) {
                // EOF — remainder is padding; zero the full chunk
                std::memset(alignedBuffer, 0, bytesToRead);
            } else if (static_cast<size_t>(bytesRead) < bytesToRead) {
                // Short read on final chunk — zero-pad the tail
                std::memset(alignedBuffer + bytesRead, 0, bytesToRead - bytesRead);
            }

            // Write loop — handles partial writes and EINTR
            size_t remainingToWrite = bytesToRead;
            char*  writePtr         = alignedBuffer;
            while (remainingToWrite > 0) {
                ssize_t written = write(dev_fd, writePtr, remainingToWrite);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error("Write error: " + std::string(strerror(errno)));
                }

                remainingToWrite -= static_cast<size_t>(written);
                writePtr         += written;

                uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize - std::min(localBytesWritten, fileSize));
                localBytesWritten                        += static_cast<size_t>(written);
                bytesInWindow                            += reportable;
                progressData[progressIndex].bytesWritten.fetch_add(reportable);
            }

            // Throttled progress + speed update
            auto now           = std::chrono::high_resolution_clock::now();
            auto msSinceUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - lastUpdate).count();
            if (msSinceUpdate >= UPDATE_INTERVAL_MS) {
                uint64_t reported = progressData[progressIndex].bytesWritten.load();
                progressData[progressIndex].progress.store(
                    static_cast<int>((static_cast<double>(reported) / fileSize) * 100));
                updateSpeed(now);
            }
        }
    } catch (...) {
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

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
        double avgSpeed = seconds > 0.0
            ? (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / seconds
            : 0.0;
        progressData[progressIndex].speed.store(avgSpeed);
        progressData[progressIndex].progress.store(100);
        progressData[progressIndex].completed.store(true);
        return true;
    }

    return false;
}
