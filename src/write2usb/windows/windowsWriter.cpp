// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libmount/libmount.h>
#include <blkid/blkid.h>

import std;
import isocmd.core.cancellation;
import isocmd.settings.settings;
import isocmd.write2usb.util.alignedBuffer;
import isocmd.write2usb.util.telemetry;
import isocmd.write2usb.util.checksum;
import isocmd.write2usb.util.threadGuard;
import isocmd.write2usb.windows.windowsWriter;
import isocmd.write2usb.windows.windowsWriterEngine;
import isocmd.write2usb.util.writeTypes;

// Declarations used only by this file (moved out of the module interface).
bool isWindowsIso(const std::string& isoPath);

namespace fs = std::filesystem;

namespace {
	// The orchestration and its mount cleanup run on the same worker. Restore
	// the previous token on exit, since thread-pool workers are reused.
	thread_local std::stop_token windowsCancelToken;

	struct WindowsCancelScope {
		std::stop_token previous;
		explicit WindowsCancelScope(const std::stop_token& token)
		    : previous(std::exchange(windowsCancelToken, token)) {}
		~WindowsCancelScope() { windowsCancelToken = std::move(previous); }
	};

	bool windowsCancelled() noexcept {
		return isUsbWriteCancelled(windowsCancelToken);
	}
} // namespace

// ---------------------------------------------------------------------------
// Global scope: device readiness
// ---------------------------------------------------------------------------

/**
 * @brief Checks if a block device is accessible and ready for I/O operations.
 */
bool isDeviceReady(const std::string& path) {

	int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1) return false;
	close(fd);
	return true;

}

/**
 * @brief Waits for a device node to appear and become ready within a timeout.
 */
bool waitForDevice(const std::string& path, int timeout_seconds = 30) {

	auto start = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() - start <
	       std::chrono::seconds(timeout_seconds)) {
		if (windowsCancelled()) return false;

		if (fs::exists(path) && fs::is_block_file(path)) {
			if (isDeviceReady(path)) return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	return false;

}

// ---------------------------------------------------------------------------
// Translation-unit private implementation
// ---------------------------------------------------------------------------

namespace {

	// ===========================================================================
	// retry:: jittered, cancellable backoff
	// ===========================================================================

	namespace retry {

		/**
		 * @brief Sleeps for a randomized backoff interval, waking early on cancel.
		 */
		void sleepWithJitter(int baseMs) {

			static thread_local std::mt19937 rng{std::random_device{}()};
			const int total =
			    baseMs + std::uniform_int_distribution<int>(0, baseMs)(rng);

			const auto end =
			    std::chrono::steady_clock::now() +
			    std::chrono::milliseconds(total);

			while (std::chrono::steady_clock::now() < end) {
				if (windowsCancelled()) return;
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

		}

		/**
		 * @brief Calls @p fn until it returns 0, with jittered linear backoff.
		 */
		template <class Fn>
		int withBackoff(Fn&& fn, int attempts = 3, int baseDelayMs = 500) {

			int rc = -1;

			for (int i = 0; i < attempts; ++i) {
				if (windowsCancelled()) return -1;

				rc = fn();
				if (rc == 0) return 0;

				if (i + 1 < attempts)
				    sleepWithJitter(baseDelayMs * (i + 1));
			}

			return rc;

		}

	} // namespace retry

	// ===========================================================================
	// proc:: spawning external commands
	// ===========================================================================

	namespace proc {

		/**
		 * @brief Runs an external command with stdout/stderr suppressed.
		 */
		int runCommand(const std::vector<std::string>& args) {

			if (args.empty() || windowsCancelled()) return -1;

			std::vector<char*> argv;
			argv.reserve(args.size() + 1);

			for (const auto& a : args)
			    argv.push_back(const_cast<char*>(a.c_str()));

			argv.push_back(nullptr);

			posix_spawn_file_actions_t fileActions;
			if (posix_spawn_file_actions_init(&fileActions) != 0)
			    return -1;

			int devNull = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (devNull < 0) {
				posix_spawn_file_actions_destroy(&fileActions);
				return -1;
			}

			posix_spawn_file_actions_adddup2(
			    &fileActions, devNull, STDOUT_FILENO);
			posix_spawn_file_actions_adddup2(
			    &fileActions, devNull, STDERR_FILENO);
			posix_spawn_file_actions_addclose(&fileActions, devNull);

			// The child runs with a fixed PATH so helper programs launched by
			// parted/mkfs resolve predictably.
			static char trustedPath[] =
			    "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
			char* const spawnEnv[] = { trustedPath, nullptr };

			pid_t pid;
			int rc = posix_spawnp(
			    &pid, argv[0], &fileActions, nullptr, argv.data(), spawnEnv);

			posix_spawn_file_actions_destroy(&fileActions);
			close(devNull);

			if (rc != 0) return -1;

			// Poll so a cancel does not leave us blocked behind a long
			// mkfs/parted child that may also be holding resources.
			int status = 0;
			bool terminated = false;

			for (;;) {
				const pid_t w = waitpid(pid, &status, WNOHANG);

				if (w == pid) break;

				if (w < 0) {
					if (errno == EINTR) continue;
					return -1;
				}

				if (!terminated && windowsCancelled()) {
					kill(pid, SIGTERM);
					terminated = true;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

			if (terminated) return -1;

			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;

		}

		/**
		 * @brief Runs an external command, retrying on failure.
		 */
		int runCommandRetry(const std::vector<std::string>& args,
		                    int attempts = 3,
		                    int baseDelayMs = 500) {

			return retry::withBackoff(
			    [&] { return runCommand(args); }, attempts, baseDelayMs);

		}

	} // namespace proc

	// ===========================================================================
	// mnt:: libmount wrappers, ScopedMount RAII, mount retries
	// ===========================================================================

	namespace mnt {

		/**
		 * @brief Mount @p src at @p target using libmnt_context.
		 */
		int libMount(const char* src,
		             const char* target,
		             const char* options = nullptr,
		             const char* fstype = nullptr) {

			libmnt_context* ctx = mnt_new_context();
			if (!ctx) return -1;

			if (src) mnt_context_set_source(ctx, src);
			if (target) mnt_context_set_target(ctx, target);

			if (options && *options)
			    mnt_context_append_options(ctx, options);

			if (fstype && *fstype)
			    mnt_context_set_fstype(ctx, fstype);

			int rc = mnt_context_mount(ctx);

			// libmount may report an error from a post-mount step (e.g. the
			// utab update) although mount(2) succeeded. Retrying then would
			// stack a second mount on the same target and leak the first.
			if (rc != 0 && mnt_context_get_status(ctx) == 1)
			    rc = 0;

			mnt_free_context(ctx);

			return rc;

		}

		/**
		 * @brief Unmounts @p path and every mount stacked on it.
		 *
		 * Uses a fresh libmount context for each attempt so loop devices and utab
		 * are cleaned up without stale context state. Normally a synchronous unmount
		 * is attempted first so the filesystem is really released when this returns,
		 * with a lazy detach as the fallback if that fails (e.g. EBUSY). On
		 * cancellation the synchronous unmount is skipped entirely and the mount is
		 * lazily detached with userspace helpers disabled, so no flush is waited on.
		 * A raw umount2() sweep then removes any remaining stacked mount layers.
		 */
		void safeUmount(const char* path) {

			auto runLibmntUmount = [&](bool lazy, bool noHelpers) -> bool {

				libmnt_context* ctx = mnt_new_context();
				if (!ctx) return false;

				mnt_context_set_target(ctx, path);
				mnt_context_enable_loopdel(ctx, true);

				if (noHelpers || lazy)
				    mnt_context_disable_helpers(ctx, true);

				if (lazy)
				    mnt_context_enable_lazy(ctx, true);

				const int rc = mnt_context_umount(ctx);
				mnt_free_context(ctx);
				return rc == 0;

			};

			const bool cancelled = windowsCancelled();

			if (cancelled) {
				// Cancelled: don't wait for the flush, detach immediately.
				runLibmntUmount(true, true);
			} else if (!runLibmntUmount(false, false)) {
				runLibmntUmount(true, true);   // EBUSY fallback
			}

			for (int i = 0; i < 8; ++i) {
				if (umount2(path, MNT_DETACH | UMOUNT_NOFOLLOW) != 0)
				    break;
			}

		}

		/**
		 * @struct ScopedMount
		 * @brief RAII wrapper to ensure automatic unmounting and directory removal.
		 */
		struct ScopedMount {
			std::string path;
			bool active = false;

			explicit ScopedMount(std::string p) : path(std::move(p)) {}

			ScopedMount(const ScopedMount&) = delete;
			ScopedMount& operator=(const ScopedMount&) = delete;
			ScopedMount(ScopedMount&&) = delete;
			ScopedMount& operator=(ScopedMount&&) = delete;

			void setMounted() {

				active = true;

			}

			~ScopedMount() {

				if (path.empty()) return;
				if (active) safeUmount(path.c_str());
				if (rmdir(path.c_str()) != 0 && errno != EBUSY) {
					// Optional: log if needed
				}

			}
		};

		/**
		 * @brief libMount() with retry and jittered backoff.
		 */
		int libMountRetry(const char* src,
		                  const char* target,
		                  const char* options = nullptr,
		                  const char* fstype = nullptr,
		                  int attempts = 3,
		                  int baseDelayMs = 500) {

			return retry::withBackoff(
			    [&] { return libMount(src, target, options, fstype); },
			    attempts, baseDelayMs);

		}

		/**
		 * @brief Mounts an NTFS partition through a specific kernel driver.
		 */
		int libMountNtfsRetry(const char* src,
		                      const char* target,
		                      const char* fstype,
		                      int attempts = 3,
		                      int baseDelayMs = 500) {

			return retry::withBackoff([&] {

				libmnt_context* ctx = mnt_new_context();
				if (!ctx) return -1;

				mnt_context_set_source(ctx, src);
				mnt_context_set_target(ctx, target);
				mnt_context_set_fstype(ctx, fstype);
				mnt_context_append_options(ctx, "noatime");
				mnt_context_disable_helpers(ctx, true);

				int rc = mnt_context_mount(ctx);

				// See libMount(): never retry a mount(2) that succeeded.
				if (rc != 0 && mnt_context_get_status(ctx) == 1)
				    rc = 0;

				mnt_free_context(ctx);

				return rc;

			}, attempts, baseDelayMs);

		}

	} // namespace mnt

	// ===========================================================================
	// iso:: ISO classification and tree enumeration
	// ===========================================================================

	namespace iso {

		// Shared definitions live in write2usb.h.
		using IsoType = write2usb_detail::IsoType;
		using IsoCopyEntry = write2usb_detail::IsoCopyEntry;

		/**
		 * @brief Classify a mounted ISO as a full Windows install or FAT-only disk.
		 */
		IsoType detectIsoType(const std::string& isoMnt) {

			return (fs::exists(isoMnt + "/sources/install.wim") ||
			        fs::exists(isoMnt + "/sources/install.esd"))
			           ? IsoType::WindowsInstall
			           : IsoType::FatOnly;

		}

		/**
		 * @brief Decide whether an ISO entry belongs on the ESP partition.
		 */
		bool isEspEntry(const fs::path& rel, IsoType isoType) {

			if (isoType == IsoType::FatOnly) return true;

			std::string top = rel.begin()->string();
			std::transform(top.begin(), top.end(), top.begin(),
			               [](unsigned char c) { return std::tolower(c); });

			static const std::unordered_set<std::string> topFolders = {
				"efi", "boot"
			};

			if (topFolders.count(top) > 0) return true;

			std::string relStr = rel.generic_string();
			std::transform(relStr.begin(), relStr.end(), relStr.begin(),
			               [](unsigned char c) { return std::tolower(c); });

			return relStr == "sources/boot.wim";

		}

		/**
		 * @brief Walk the ISO tree and classify each entry into NTFS and ESP lists.
		 */
		bool enumerateIsoEntries(const std::string& isoMnt,
		                         IsoType isoType,
		                         const std::string& fatMnt,
		                         const std::string& ntfsMnt,
		                         std::vector<IsoCopyEntry>& ntfsEntries,
		                         std::vector<IsoCopyEntry>& espEntries,
		                         std::uint64_t& totalBytes) {

			try {
				for (const auto& entry :
				     fs::recursive_directory_iterator(isoMnt)) {
					if (windowsCancelled()) return false;
					// Reject symlinks outright — a malicious ISO (Rock Ridge/UDF)
					// could embed a symlink pointing at an arbitrary host path.
					if (fs::is_symlink(entry.symlink_status()))
					    continue;

					const fs::path rel = fs::relative(entry.path(), isoMnt);
					const bool toESP = isEspEntry(rel, isoType);
					const fs::path dest =
					    fs::path(toESP ? fatMnt : ntfsMnt) / rel;
					const std::uint64_t sz =
					    entry.is_regular_file() ? entry.file_size() : 0;

					totalBytes += sz;

					IsoCopyEntry e{
						entry.path(), dest, sz, entry.is_directory()
					};

					if (toESP)
					    espEntries.push_back(std::move(e));
					else
					    ntfsEntries.push_back(std::move(e));
				}
			} catch (...) {
				return false;
			}

			return true;

		}

	} // namespace iso

	// IsoType is used throughout the remaining namespaces and public entry point.
	using IsoType = write2usb_detail::IsoType;

	// The copy engine is now implemented in windowsWriterEngine.cpp.
	namespace copy = windowsWriter_engine::copy;

	// ===========================================================================
	// disk:: wipe, partition, format and mount the target device
	// ===========================================================================

	namespace disk {

		/**
		 * @brief Probes /sys/module to determine the best available write-capable
		 *        NTFS kernel driver currently active on the host platform.
		 */
		std::string getBestNtfsDriver() {

			if (std::filesystem::exists("/sys/module/ntfs"))
			    return "ntfs";

			proc::runCommand({"modprobe", "ntfs"});
			if (std::filesystem::exists("/sys/module/ntfs"))
			    return "ntfs";

			proc::runCommand({"modprobe", "ntfs3"});
			if (std::filesystem::exists("/sys/module/ntfs3"))
			    return "ntfs3";

			return {};

		}

		/**
		 * @brief Erase all filesystem/partition signatures on @p device.
		 */
		bool wipeDeviceSignatures(const std::string& device) {

			if (windowsCancelled()) return false;
			blkid_probe pr =
			    blkid_new_probe_from_filename(device.c_str());
			if (!pr) return false;

			blkid_probe_enable_superblocks(pr, true);
			blkid_probe_set_superblocks_flags(
			    pr, BLKID_SUBLKS_MAGIC | BLKID_SUBLKS_TYPE);

			blkid_probe_enable_partitions(pr, true);
			blkid_probe_set_partitions_flags(pr, BLKID_PARTS_MAGIC);

			// Loop until no more signatures are found.
			while (!windowsCancelled() && blkid_do_probe(pr) == 0) {
				if (windowsCancelled()) break;
				blkid_do_wipe(pr, false /* not dry-run */);
			}

			blkid_free_probe(pr);
			return !windowsCancelled();

		}

		/**
		 * @brief Wipe device signatures, partition via parted and derive partition
		 *        node paths with waitForDevice.
		 */
		bool setupPartitionedDevice(const std::string& device,
		                           IsoType isoType,
		                           std::string& fatPart,
		                           std::string& ntfsPart) {

			if (!wipeDeviceSignatures(device)) return false;

			int partResult = -1;

			if (isoType == IsoType::WindowsInstall) {
				partResult = proc::runCommandRetry({
					"parted", "-s", device,
					"mklabel", "gpt",
					"mkpart", "ESP", "fat32", "1MiB", "1025MiB",
					"mkpart", "DATA", "ntfs", "1025MiB", "100%",
					"set", "1", "esp", "on",
					"set", "1", "boot", "on"
				});
			} else {
				partResult = proc::runCommandRetry({
					"parted", "-s", device,
					"mklabel", "gpt",
					"mkpart", "WINPE", "fat32", "1MiB", "100%",
					"set", "1", "esp", "on",
					"set", "1", "boot", "on"
				});
			}

			if (partResult != 0) return false;
			if (!waitForDevice(device)) return false;

			auto derive = [&](int n) -> std::string {

				std::string t = device + std::to_string(n);
				return waitForDevice(t, 30) ? t : std::string{};

			};

			fatPart = derive(1);
			ntfsPart = (isoType == IsoType::WindowsInstall)
			               ? derive(2)
			               : std::string{};

			if (fatPart.empty()) return false;

			if (isoType == IsoType::WindowsInstall && ntfsPart.empty())
			    return false;

			return true;

		}

		/**
		 * @brief Format the FAT32 partition and, for WindowsInstall, the NTFS one.
		 */
		bool formatTargetPartitions(const std::string& fatPart,
		                            const std::string& ntfsPart,
		                            IsoType isoType) {

			if (proc::runCommandRetry({
					"mkfs.fat", "-F", "32", "-n",
					(isoType == IsoType::WindowsInstall ? "WINBOOT" : "WINPE"),
					fatPart
				}) != 0) {
				return false;
			}

			if (isoType == IsoType::WindowsInstall) {
				if (proc::runCommandRetry({
						"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA",
						ntfsPart
					}) != 0) {
					return false;
				}
			}

			return true;

		}

		/**
		 * @brief Mount FAT32 and (WindowsInstall only) NTFS target filesystems.
		 */
		bool mountTargetFilesystems(IsoType isoType,
		                            const std::string& fatPart,
		                            const std::string& ntfsPart,
		                            const std::string& fatMnt,
		                            const std::string& ntfsMnt,
		                            mnt::ScopedMount& fatScope,
		                            mnt::ScopedMount& ntfsScope,
		                            std::string& ntfsDriver) {

			if (mnt::libMountRetry(
			        fatPart.c_str(), fatMnt.c_str(), "noatime") != 0)
			    return false;

			fatScope.setMounted();

			if (isoType == IsoType::WindowsInstall) {
				ntfsDriver = getBestNtfsDriver();
				if (ntfsDriver.empty()) return false;

				if (mnt::libMountNtfsRetry(
				        ntfsPart.c_str(), ntfsMnt.c_str(),
				        ntfsDriver.c_str()) != 0)
				    return false;

				ntfsScope.setMounted();
			}

			return true;

		}

		/**
		 * @brief syncfs() the filesystem mounted at @p mountPath.
		 *
		 * Both NTFS and FAT32 may use direct or buffered payload writes.
		 * A deferred I/O error (e.g. the stick failing or being pulled) can
		 * surface here.
		 *
		 * @return true if the filesystem was opened and flushed successfully.
		 */
		bool syncMount(const std::string& mountPath) {

			const int fd = open(mountPath.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) return false;

			const bool ok = (syncfs(fd) == 0);
			close(fd);
			return ok;

		}

	} // namespace disk

	// ===========================================================================
	// telemetry:: progress / speed monitor thread
	// ===========================================================================

	namespace telemetry {

		/**
		 * @brief Polls progress and speed into @p info.
		 */
		void runProgressMonitor(std::atomic<std::uint64_t>& accumulator,
		                        std::atomic<bool>& active,
		                        std::uint64_t totalBytes,
		                        ProgressInfo& info) {

			write_telemetry::SpeedEstimator estimator(write_telemetry::Clock::now(),
			    write_telemetry::windowsStallTimeoutSeconds);
			while (active.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				const auto now = write_telemetry::Clock::now();
				const std::uint64_t currentWritten = accumulator.load();
				info.bytesWritten.store(currentWritten);
				info.progress.store(write_telemetry::transferPercent(currentWritten, totalBytes));
				if (auto speed = estimator.sample(currentWritten, now))
				    info.speed.store(*speed);
			}

		}

	} // namespace telemetry

	// ===========================================================================
	// win_iso_steps:: orchestration steps used by writeWindowsIsoToDevice
	// ===========================================================================

	namespace win_iso_steps {

		using EntryVec = std::vector<iso::IsoCopyEntry>;

		bool isCancelled() {

			return windowsCancelled();

		}

		// Centralises the genuine-error vs. user-cancellation failure policy.
		struct FailureReporter {
			ProgressInfo& slot;

			bool fail() const {

				slot.failed.store(true);
				return false;

			}

			bool failUnlessCancelled() const {

				if (isCancelled()) return false;
				return fail();

			}
		};

		// Mounts the ISO read-only; arms the scope only on success.
		bool mountIso(const std::string& isoPath,
		              const char* isoMntBuf,
		              mnt::ScopedMount& isoScope) {

			if (mnt::libMountRetry(
			        isoPath.c_str(), isoMntBuf, "ro,loop") != 0)
			    return false;

			isoScope.setMounted();
			return true;

		}

		// Wipe, partition and format.
		bool prepareDevice(const std::string& device,
		                   IsoType isoType,
		                   std::string& fatPart,
		                   std::string& ntfsPart) {

			if (!disk::setupPartitionedDevice(
			        device, isoType, fatPart, ntfsPart))
			    return false;

			if (!disk::formatTargetPartitions(
			        fatPart, ntfsPart, isoType))
			    return false;

			return !isCancelled();

		}

		// The NTFS temp dir only exists for WindowsInstall images.
		bool makeNtfsDirIfNeeded(IsoType isoType, char* ntfsMntBuf) {

			return isoType != IsoType::WindowsInstall ||
			       mkdtemp(ntfsMntBuf) != nullptr;

		}

		std::string ntfsScopePath(IsoType isoType,
		                          const char* ntfsMntBuf) {

			return isoType == IsoType::WindowsInstall
			           ? std::string(ntfsMntBuf)
			           : std::string{};

		}

		bool mountTargets(IsoType isoType,
		                  const std::string& fatPart,
		                  const std::string& ntfsPart,
		                  const std::string& fatMnt,
		                  const std::string& ntfsMnt,
		                  mnt::ScopedMount& fatScope,
		                  mnt::ScopedMount& ntfsScope,
		                  std::string& ntfsDriver) {

			if (!disk::mountTargetFilesystems(
			        isoType, fatPart, ntfsPart,
			        fatMnt, ntfsMnt, fatScope, ntfsScope, ntfsDriver))
			    return false;

			return !isCancelled();

		}

		// Fails if enumeration fails or the ISO contributes no bytes.
		bool enumerateNonEmpty(const std::string& isoMnt,
		                       IsoType isoType,
		                       const std::string& fatMnt,
		                       const std::string& ntfsMnt,
		                       EntryVec& ntfsEntries,
		                       EntryVec& espEntries,
		                       std::uint64_t& totalBytes) {

			if (!iso::enumerateIsoEntries(
			        isoMnt, isoType, fatMnt, ntfsMnt,
			        ntfsEntries, espEntries, totalBytes))
			    return false;

			return totalBytes != 0;

		}

		// Flushes every target filesystem (both are attempted even if the first
		// fails). Returns false if any flush failed or a cancel was observed.
		bool finalFlush(IsoType isoType,
		                const std::string& ntfsMnt,
		                const std::string& fatMnt) {

    		if (isCancelled()) return false;

    		bool ok = true;

    		if (isoType == IsoType::WindowsInstall)
                ok = disk::syncMount(ntfsMnt) && ok;

    		if (isCancelled()) return false;

    		ok = disk::syncMount(fatMnt) && ok;

    		return ok && !isCancelled();

		}

		// NTFS payload first, then ESP, so the bootloader only lands after
		// the data it references is already on disk.
		template <class RunPass>
		bool runCopyPasses(RunPass&& runPass,
		                   IsoType isoType,
		                   const std::string& ntfsMnt,
		                   const std::string& fatMnt,
		                   EntryVec& ntfsEntries,
		                   EntryVec& espEntries) {

			if (!runPass(ntfsEntries)) return false;
			if (!runPass(espEntries)) return false;

			if (isCancelled()) return false;

			return finalFlush(isoType, ntfsMnt, fatMnt);

		}

		// Verification must not read dirty or still-cached file data from the
		// preceding mount. No lazy unmount is accepted on this path.
		bool reopenReadOnly(const std::string& partition, mnt::ScopedMount& scope, const char* filesystemType) {

			if (isCancelled()) return false;
			int rc;
			do { rc = umount2(scope.path.c_str(), UMOUNT_NOFOLLOW); }
			while (rc != 0 && errno == EINTR && !isCancelled());
			if (rc != 0) return false;
			scope.active = false;
			if (isCancelled()) return false;
			const int fd = open(partition.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) return false;
			do { rc = ioctl(fd, BLKFLSBUF, 0); }
			while (rc != 0 && errno == EINTR && !isCancelled());
			const bool closed = close(fd) == 0;
			if (rc != 0 || !closed || isCancelled()) return false;
			// The writer already selected and loaded the kernel filesystem
			// drivers. Readback needs no mount helper or external command.
			if (retry::withBackoff([&] {

					return mount(partition.c_str(), scope.path.c_str(), filesystemType,
					             MS_RDONLY | MS_NOATIME, nullptr);

				}) != 0) return false;
			scope.setMounted();
			return !isCancelled();

		}

		bool verifyEntries(const EntryVec& entries, const std::vector<std::uint64_t>& checksums,
		                   char* buffer, std::size_t bufferSize, usb_verify::ReadbackProgress& progress) {

			std::size_t index = 0;
			for (const auto& entry : entries) {
				if (isCancelled()) return false;
				if (entry.isDir) {
					std::error_code ec;
					if (!fs::is_directory(fs::symlink_status(entry.dst, ec)) || ec) return false;
				} else {
					if (index >= checksums.size() || !usb_verify::verifyFile(entry.dst, entry.size,
					      checksums[index++], buffer, bufferSize, progress)) return false;
				}
			}
			return index == checksums.size();

		}

		// Non-owning references to the prepared target filesystems. The writer
		// retains ownership of both mount scopes throughout copying and readback.
		struct MountedTargets {
			IsoType isoType;
			const std::string& fatPart;
			const std::string& ntfsPart;
			const std::string& fatMnt;
			const std::string& ntfsMnt;
			mnt::ScopedMount& fatScope;
			mnt::ScopedMount& ntfsScope;
			const std::string& ntfsDriver;
		};

		bool verifyCopiedEntries(const MountedTargets& targets,
		                         const EntryVec& ntfsEntries, const EntryVec& espEntries,
		                         const std::vector<std::uint64_t>& ntfsChecksums,
		                         const std::vector<std::uint64_t>& espChecksums,
		                         const AlignedBuffer& ioBuf,
		                         usb_verify::ReadbackProgress& readback) {

			if (targets.isoType == IsoType::WindowsInstall &&
			    !reopenReadOnly(targets.ntfsPart, targets.ntfsScope, targets.ntfsDriver.c_str()))
			    return false;

			return reopenReadOnly(targets.fatPart, targets.fatScope, "vfat") &&
			       verifyEntries(ntfsEntries, ntfsChecksums, ioBuf.data, ioBuf.size, readback) &&
			       verifyEntries(espEntries, espChecksums, ioBuf.data, ioBuf.size, readback) &&
			       readback.finish();

		}

		double averageSpeedMBps(std::uint64_t totalBytes, double seconds) {

			if (seconds <= 0.0) return 0.0;

			return (static_cast<double>(totalBytes) /
			        (1024.0 * 1024.0)) / seconds;

		}

		void reportSuccess(ProgressInfo& slot) {

			slot.progress.store(100);
			slot.completed.store(true);

		}

	} // namespace win_iso_steps

} // namespace

// ---------------------------------------------------------------------------
// Global scope: public entry points
// ---------------------------------------------------------------------------

/**
 * @brief Detects whether an ISO image is Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath) {

	char tmpDir[] = "/tmp/iso_probe_XXXXXX";
	if (!mkdtemp(tmpDir)) return false;

	auto cleanup = [&]() {

		mnt::safeUmount(tmpDir);
		rmdir(tmpDir);

	};

	if (mnt::libMount(isoPath.c_str(), tmpDir, "ro,loop") != 0) {
		rmdir(tmpDir);
		return false;
	}

	const bool hasBootWim =
	    fs::exists(std::string(tmpDir) + "/sources/boot.wim");

	const bool hasBootMgr =
	    fs::exists(std::string(tmpDir) + "/bootmgr") ||
	    fs::exists(std::string(tmpDir) + "/bootmgr.efi");

	cleanup();
	return hasBootWim && hasBootMgr;

}

/**
 * @brief Copy Windows ISO contents to prepared filesystems and optionally
 *        verify the written data using CRC-64 readback.
 *
 * @return true on successful copying and optional verification;
 *         false on error or cancellation.
 */
static bool copyAndVerifyWindowsIso(
        const std::string& isoMnt,
        const win_iso_steps::MountedTargets& targets,
        ProgressInfo& slot) {

	namespace steps = win_iso_steps;

	const steps::FailureReporter report{slot};

	std::vector<iso::IsoCopyEntry> ntfsEntries, espEntries;
	std::uint64_t totalBytes = 0;

	if (!steps::enumerateNonEmpty(
	        isoMnt, targets.isoType, targets.fatMnt, targets.ntfsMnt,
	        ntfsEntries, espEntries, totalBytes))
	    return report.failUnlessCancelled();

	std::atomic<std::uint64_t> totalBytesWrittenAccumulator{0};
	std::atomic<bool> monitoringActive{true};

	const auto startTime = std::chrono::high_resolution_clock::now();

	std::thread progressMonitorThread([&, totalBytes]() {

		telemetry::runProgressMonitor(
		    totalBytesWrittenAccumulator, monitoringActive,
		    totalBytes, slot);

	});

	// Ensures the thread is stopped and joined on every return path.
	ThreadGuard threadGuard(progressMonitorThread, monitoringActive);

	constexpr std::size_t bufferSize = std::max(copy::kBufferSize, copy::kFatBufferSize);

	AlignedBuffer ioBuf(bufferSize, copy::kBufferAlign);
	if (!ioBuf)
	    return report.failUnlessCancelled();

	std::vector<std::uint64_t> ntfsChecksums, espChecksums;
	const bool buffered = slot.bufferedWrites ? *slot.bufferedWrites : write2usbBufferedEnabled();

	auto runPass = [&](std::vector<iso::IsoCopyEntry>& entries) {

		auto* checksums = slot.verifyRequested
		    ? (&entries == &ntfsEntries ? &ntfsChecksums : &espChecksums)
		    : nullptr;

		return copy::processEntries(
		    entries, targets.isoType, targets.ntfsMnt,
		    &entries == &ntfsEntries ? copy::kBufferSize : copy::kFatBufferSize, ioBuf.data,
		    totalBytesWrittenAccumulator, checksums, buffered, slot.cancelToken);

	};

	if (!steps::runCopyPasses(
	        runPass, targets.isoType, targets.ntfsMnt, targets.fatMnt,
	        ntfsEntries, espEntries))
	    return report.failUnlessCancelled();

	// Copying and the final filesystem flush have succeeded.
	slot.writeCompleted.store(true);
	threadGuard.stop();
	slot.bytesWritten.store(totalBytesWrittenAccumulator.load());

	if (steps::isCancelled())
	    return false;

	const auto totalElapsed =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::high_resolution_clock::now() - startTime);

	const double seconds = totalElapsed.count() / 1000.0;
	slot.speed.store(steps::averageSpeedMBps(totalBytes, seconds));

	if (slot.verifyRequested) {
		usb_verify::ReadbackProgress readback(slot, totalBytes);

		if (!steps::verifyCopiedEntries(
		        targets, ntfsEntries, espEntries,
		        ntfsChecksums, espChecksums, ioBuf, readback))
		    return report.failUnlessCancelled();

		return true;
	}

	steps::reportSuccess(slot);

	return true;

}

/**
 * @brief Wipe and repartition a block device, write a Windows-family ISO
 *        using direct or buffered NTFS and FAT32 data writes,
 *        and optionally verify the written data.
 *
 * @param isoPath       Path to the source ISO file.
 * @param device        Target block device. Will be wiped.
 * @param progressIndex Index into progressData for live status reporting.
 * @param progressData  Shared progress tracking array.
 * @return true on successful writing and optional verification;
 *         false on error, truncated source, or cancellation.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             std::size_t progressIndex,
                             std::vector<ProgressInfo>& progressData) {

	namespace steps = win_iso_steps;

	ProgressInfo& slot = progressData[progressIndex];
	const WindowsCancelScope cancelScope(slot.cancelToken);
	if (windowsCancelled()) return false;
	const steps::FailureReporter report{slot};

	char isoMntBuf[] = "/tmp/win_iso_XXXXXX";
	if (!mkdtemp(isoMntBuf))
	    return report.fail();

	mnt::ScopedMount isoScope(isoMntBuf);

	if (!steps::mountIso(isoPath, isoMntBuf, isoScope))
	    return report.failUnlessCancelled();

	const std::string isoMnt = isoMntBuf;
	const IsoType isoType = iso::detectIsoType(isoMnt);

	std::string fatPart, ntfsPart;
	if (!steps::prepareDevice(device, isoType, fatPart, ntfsPart))
	    return report.failUnlessCancelled();

	char fatMntBuf[] = "/tmp/win_fat_XXXXXX";
	char ntfsMntBuf[] = "/tmp/win_ntfs_XXXXXX";

	if (!mkdtemp(fatMntBuf))
	    return report.fail();

	mnt::ScopedMount fatScope(fatMntBuf);

	if (!steps::makeNtfsDirIfNeeded(isoType, ntfsMntBuf))
	    return report.fail();

	mnt::ScopedMount ntfsScope(
	    steps::ntfsScopePath(isoType, ntfsMntBuf));

	const std::string fatMnt = fatMntBuf;
	const std::string ntfsMnt =
	    (isoType == IsoType::WindowsInstall)
	        ? ntfsMntBuf : std::string{};

	std::string ntfsDriver;

	if (!steps::mountTargets(
	        isoType, fatPart, ntfsPart,
	        fatMnt, ntfsMnt, fatScope, ntfsScope, ntfsDriver))
	    return report.failUnlessCancelled();

	// Mount scopes remain alive throughout copying and verification,
	// then clean up in reverse order: NTFS -> FAT32 -> ISO.
	const steps::MountedTargets targets{
	    isoType, fatPart, ntfsPart, fatMnt, ntfsMnt,
	    fatScope, ntfsScope, ntfsDriver};

	return copyAndVerifyWindowsIso(isoMnt, targets, slot);

}
