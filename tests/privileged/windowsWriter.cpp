// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <blkid/blkid.h>
#include "../common/testHarness.h"

import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.core.cancellation;
import isocmd.settings.settings;

namespace isocmd_tests {
	namespace {
		constexpr std::uint64_t capacity = 1280ULL * 1024 * 1024;

		struct Mounted {
			fs::path path;
			bool active = false;

			explicit Mounted(fs::path p) : path(std::move(p)) {

				CHECK(fs::create_directory(path));

			}

			~Mounted() {

				if (active) ::umount2(path.c_str(), MNT_DETACH);
				std::error_code ec;
				fs::remove(path, ec);

			}

			void open(const fs::path& device, const char* type) {

				CHECK(::mount(device.c_str(), path.c_str(), type, MS_RDONLY | MS_NOATIME, nullptr) == 0);
				active = true;

			}

			void close() {

				CHECK(::umount2(path.c_str(), UMOUNT_NOFOLLOW) == 0); active = false;

			}
		};

		std::string deviceProperty(const fs::path& device, const char* key) {

			auto probe = blkid_new_probe_from_filename(device.c_str());
			CHECK(probe);

			struct Guard { blkid_probe p; ~Guard() {

					blkid_free_probe(p);

				} } guard{probe};

			CHECK(blkid_probe_enable_partitions(probe, 1) == 0);
			CHECK(blkid_probe_enable_superblocks(probe, 1) == 0);
			CHECK(blkid_probe_set_superblocks_flags(probe, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL) == 0);
			CHECK(blkid_do_safeprobe(probe) == 0);
			const char* value = nullptr;
			std::size_t len = 0;
			CHECK(blkid_probe_lookup_value(probe, key, &value, &len) == 0 && value);
			// libblkid reports the terminating NUL in the length for strings.
			return std::string(value, len && value[len - 1] == '\0' ? len - 1 : len);

		}

		// Independently validate the final partition against its kernel sysfs parent.
		void checkPartition(const fs::path& parent, const fs::path& alias, int number) {

			const auto child = parent / (parent.filename().string() + "p" + std::to_string(number));
			CHECK(fs::exists(child / "dev"));
			CHECK(fs::canonical(child).parent_path() == parent);
			unsigned int part = 0, maj = 0, min = 0;
			char colon = 0;
			std::ifstream partFile(child / "partition"), devFile(child / "dev");
			CHECK((partFile >> part) && part == static_cast<unsigned>(number));
			CHECK((devFile >> maj >> colon >> min) && colon == ':');
			const dev_t id = makedev(maj, min);
			CHECK(fs::canonical(fs::path("/sys/dev/block") / (std::to_string(maj) + ':' + std::to_string(min))) == child);
			struct stat st{};
			CHECK(stat(alias.c_str(), &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == id);

		}

		void checkTree(const fs::path& source, const fs::path& fat, const fs::path& ntfs, bool full) {

			std::set<fs::path> fatFiles;
			for (const auto& entry : fs::recursive_directory_iterator(source)) {
				const auto rel = entry.path().lexically_relative(source);
				const auto top = rel.begin()->string();
				const bool onFat = !full || top == "efi" || top == "boot" || rel == "sources/boot.wim";
				const auto& root = onFat ? fat : ntfs;
				const auto target = root / rel;
				if (entry.is_directory()) {
					CHECK(fs::is_directory(target));
				} else {
					CHECK(entry.is_regular_file());
					CHECK(fs::is_regular_file(target));
					CHECK(readFile(target) == readFile(entry.path()));
					if (onFat) fatFiles.insert(rel);
					if (full) CHECK(!fs::exists((onFat ? ntfs : fat) / rel));
				}
			}
			// FAT has no extra regular files. NTFS may expose filesystem metadata
			// depending on its kernel driver, so check the exact placement of every
			// fixture file above instead of assuming an empty metadata namespace.
			for (const auto& entry : fs::recursive_directory_iterator(fat)) {
				if (entry.is_regular_file()) CHECK(fatFiles.contains(entry.path().lexically_relative(fat)));
			}

		}
	} // namespace

	static void checkWindowsDevice(const std::string& loop, struct stat& ds) {

		CHECK(loop.starts_with("/dev/loop") && loop.size() > 9 &&
		      std::all_of(loop.begin()+9, loop.end(), [](char c) { return c >= '0' && c <= '9'; }));
		CHECK(lstat(loop.c_str(), &ds) == 0 && S_ISBLK(ds.st_mode) && major(ds.st_rdev) == 7);

	}

	static void checkWindowsSandbox(const fs::path& sandbox, const fs::path& backing, const char* parentNs, struct stat& bs) {

		struct stat ss{}, ns{};
		CHECK(geteuid() == 0 && fs::current_path() == sandbox);
		CHECK(stat("/proc/self/ns/mnt", &ns) == 0 && std::to_string(ns.st_ino) != parentNs);
		CHECK(lstat(sandbox.c_str(), &ss) == 0 && S_ISDIR(ss.st_mode) && ss.st_uid == 0 && (ss.st_mode & 077) == 0);
		CHECK(lstat(backing.c_str(), &bs) == 0 && S_ISREG(bs.st_mode) && bs.st_uid == 0 && bs.st_nlink == 1 && bs.st_size == capacity);

	}

	static void checkWindowsLoop(int fd, const struct stat& bs) {

		struct loop_info64 info{};
		std::uint64_t size = 0;
		CHECK(ioctl(fd, LOOP_GET_STATUS64, &info) == 0 && info.lo_inode == bs.st_ino &&
		      info.lo_device == static_cast<std::uint64_t>(bs.st_dev) && info.lo_offset == 0 && info.lo_sizelimit == 0 &&
		      (info.lo_flags & LO_FLAGS_PARTSCAN) && !(info.lo_flags & LO_FLAGS_READ_ONLY));
		CHECK(ioctl(fd, BLKGETSIZE64, &size) == 0 && size == capacity);

	}

	static void createPartitionAliases(const std::string& loop, const fs::path& device, const fs::path& fatPart, const fs::path& ntfsPart, bool full) {

		// The real writer derives device+N; the kernel names loop partitions
		// loopXpN. Link to the pinned loop's exact kernel node names. A fresh
		// loop must have no existing partition nodes; devtmpfs creates them
		// as parted updates the kernel table. Links follow the current nodes
		// even if a partition reread assigns a different dynamic minor.
		fs::create_symlink(loop, device);
		for (int n : {1, 2}) {
			const auto target = loop + "p" + std::to_string(n);
			struct stat existing{};
			CHECK(lstat(target.c_str(), &existing) == -1 && errno == ENOENT);
			if (n == 1 || full) fs::create_symlink(target, n == 1 ? fatPart : ntfsPart);
		}

	}

	static void checkWindowsLayout(const fs::path& sys, const fs::path& device, const fs::path& fatPart, bool full) {

		CHECK(deviceProperty(device, "PTTYPE") == "gpt");
		checkPartition(sys, fatPart, 1);
		CHECK(deviceProperty(fatPart, "TYPE") == "vfat");
		CHECK(deviceProperty(fatPart, "LABEL") == (full ? "WINBOOT" : "WINPE"));
		const auto second = sys / (sys.filename().string() + "p2");
		CHECK(fs::exists(second) == full);
		std::size_t partitions = 0;
		for (const auto& entry : fs::directory_iterator(sys))
		    if (fs::exists(entry.path() / "partition")) ++partitions;
		CHECK(partitions == (full ? 2u : 1u));

	}

	static void checkWindowsVolumes(const fs::path& sandbox, const fs::path& source, const fs::path& sys, const fs::path& fatPart, const fs::path& ntfsPart, bool full, const std::string& before) {

		// Independently mount the resulting volumes read-only and compare every
		// fixture file byte-for-byte, without using the CRC implementation.
		Mounted fat(sandbox / "windows-check-fat"), ntfs(sandbox / "windows-check-ntfs");
		fat.open(fatPart, "vfat");
		if (full) {
			checkPartition(sys, ntfsPart, 2);
			CHECK(deviceProperty(ntfsPart, "TYPE") == "ntfs" && deviceProperty(ntfsPart, "LABEL") == "WINDATA");
			const char* driver = fs::exists("/sys/module/ntfs") ? "ntfs" : "ntfs3";
			ntfs.open(ntfsPart, driver);
		}
		checkTree(source, fat.path, ntfs.path, full);
		if (full) ntfs.close();
		fat.close();
		CHECK(readFile("/proc/self/mountinfo") == before);

	}

	static void checkWindowsWrite(const fs::path& iso, const fs::path& device, std::uint64_t total, const std::string& before) {

		std::vector<ProgressInfo> progress;
		progress.emplace_back(iso.filename().string(), device.string(), "test", iso.string());
		auto& slot = progress.front();
		slot.verifyRequested = true;
		GlobalState::g_operationCancelled = false;
		// Call the real dispatcher too: a false raw-image classification cannot
		// satisfy the partition, labels, contents or exact byte-count assertions.
		const bool success = writeIsoToDevice(iso.string(), device.string(), 0, progress);
		CHECK(readFile("/proc/self/mountinfo") == before);
		CHECK(success && slot.writeCompleted && slot.completed && !slot.failed && slot.verifying && slot.progress == 100);
		CHECK(slot.bytesWritten == total && slot.bytesVerified == total);

	}

	static fs::path checkWindowsFixtures(const fs::path& devDir, const fs::path& iso, const fs::path& source, const std::string& loop, const struct stat& ds) {

		struct stat dd{};
		CHECK(lstat(devDir.c_str(), &dd) == 0 && S_ISDIR(dd.st_mode) && dd.st_uid == 0 && (dd.st_mode & 077) == 0 && fs::is_empty(devDir));
		CHECK(fs::is_regular_file(iso) && !fs::is_symlink(iso) && fs::is_directory(source));
		const auto sys = fs::canonical(fs::path("/sys/dev/block") /
		    (std::to_string(major(ds.st_rdev)) + ':' + std::to_string(minor(ds.st_rdev))));
		CHECK(sys.filename() == fs::path(loop).filename() && fs::exists(sys / "loop/backing_file"));
		CHECK(!fs::exists(sys / (sys.filename().string() + "p1")) && !fs::exists(sys / (sys.filename().string() + "p2")));
		return sys;

	}

	int windowsWriterTest(const std::string& loop, const std::string& layout, const std::string& mode) {

		try {
			CHECK(layout == "full" || layout == "fat-only");
			const bool full = layout == "full";
			CHECK(mode == "direct" || mode == "buffered");
			const char* env = std::getenv("ISOCMD_TEST_SANDBOX"); CHECK(env);
			const char* parentNs = std::getenv("ISOCMD_WINDOWS_PARENT_NS"); CHECK(parentNs);
			const auto sandbox = fs::canonical(env);
			const auto backing = sandbox / "windows-backing.img";
			const auto devDir = sandbox / "windows-dev";
			const auto device = devDir / "target";
			const auto fatPart = devDir / "target1", ntfsPart = devDir / "target2";
			const auto iso = sandbox / ("windows-" + layout + ".iso");
			const auto source = sandbox / ("windows-" + layout + "-tree");
			struct stat bs{}, ds{};
			checkWindowsSandbox(sandbox, backing, parentNs, bs);
			checkWindowsDevice(loop, ds);
			ReadFd pinned(loop);
			checkWindowsLoop(pinned.fd, bs);
			const auto sys = checkWindowsFixtures(devDir, iso, source, loop, ds);
			createPartitionAliases(loop, device, fatPart, ntfsPart, full);
			std::uint64_t total = 0;
			for (const auto& entry : fs::recursive_directory_iterator(source))
			    if (entry.is_regular_file()) total += entry.file_size();
			CHECK(total > 4 * 1024 * 1024);
			const auto before = readFile("/proc/self/mountinfo");
			// Each invocation runs in its own process with isolated configuration.
			ConfigCaches::g_configCache["buffered_writes"] = mode == "buffered" ? "on" : "off";
			CHECK(write2usbBufferedEnabled() == (mode == "buffered"));
			checkWindowsWrite(iso, device, total, before);
			checkWindowsLayout(sys, device, fatPart, full);
			checkWindowsVolumes(sandbox, source, sys, fatPart, ntfsPart, full, before);
			std::cout << "PASS: Windows " << (full ? "FAT32 + NTFS" : "FAT-only")
			          << " (" << mode << ") verified write, partition layout, readback bytes and cleanup\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: Windows " << layout << " (" << mode << ") verification: " << e.what() << '\n';
			return 1;
		}

	}
} // namespace isocmd_tests
