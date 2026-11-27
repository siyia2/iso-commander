// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks busy and mounted indicators, device validation and in-flight counter parsing.
 */

#include "../common/testHarness.h"

import isocmd.core.uiState;
import isocmd.ui.display.themes;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.drainingState;

namespace isocmd_tests {

	namespace {
		struct BusyFixture {
			TempDir tmp;
			const DeviceDiscoveryPaths paths{tmp.path/"sys/block", tmp.path/"dev", tmp.path/"mounts"};
			const std::string device = (paths.dev/"sda").string();
			const fs::path inflight = paths.sysBlock/"sda/inflight";

			BusyFixture() {
				const auto usb = tmp.path/"usb";
				const auto node = usb/"block/sda";
				fs::create_directories(node);
				fs::create_directories(paths.sysBlock);
				fs::create_directories(paths.dev);
				writeFile(usb/"idVendor", "1234\n");
				writeFile(node/"removable", "1\n");
				writeFile(node/"size", "4096\n");
				fs::create_directory_symlink(node, paths.sysBlock/"sda");
				writeFile(device, ""); // Regular file, never a real block device.
				writeFile(paths.mounts, "");
				writeFile(inflight, "0 0\n");
			}

			void mounted(bool value) const {
				writeFile(paths.mounts, value ? device+"1 /mnt/test ext4 rw 0 0\n" : "");
			}

			std::string_view label() const {
				return WriteValidation::deviceStatusLabel(device, paths);
			}
		};

		struct Reservation {
			const std::string& device;
			explicit Reservation(const std::string& path) : device(path) { g_drainingManager.reserveDevice(device); }
			~Reservation() { g_drainingManager.releaseDevice(device); }
		};

		struct ModeRestore {
			const bool previous = GlobalState::isNonInteractive.load();
			~ModeRestore() { GlobalState::isNonInteractive.store(previous); }
		};

		void checkReady(const BusyFixture& f) {
			std::uint64_t size = 0;
			CHECK(!WriteValidation::checkDevice(getWriteTheme(), f.device, f.device, size, f.paths));
			CHECK(size == 4096ULL * 512);
		}

		void checkRejected(const BusyFixture& f, std::string_view reason) {
			std::uint64_t size = 0;
			const auto error = WriteValidation::checkDevice(getWriteTheme(), f.device, f.device, size, f.paths);
			CHECK(error && error->find(reason) != std::string::npos);
			CHECK(size == 0);
		}
	}

	/**
	 * @brief Checks displayed device status and the priority of mounted over busy.
	 */
	void usbBusyIndicators() {
		BusyFixture f;
		CHECK(f.label().empty());
		writeFile(f.inflight, "3 0\n");
		CHECK(f.label().empty());
		writeFile(f.inflight, "0 2\n");
		CHECK(f.label() == "[BUSY]");
		f.mounted(true);
		CHECK(f.label() == "[MOUNTED]");
		writeFile(f.inflight, "0 0\n");
		CHECK(f.label() == "[MOUNTED]");
		{
			Reservation reserved(f.device);
			CHECK(f.label() == "[MOUNTED]");
			f.mounted(false);
			CHECK(f.label() == "[BUSY]");
		}
		CHECK(f.label().empty());
	}

	/**
	 * @brief Checks busy-device rejection and recovery in interactive and CLI validation.
	 */
	void usbBusyValidation() {
		BusyFixture f;
		ModeRestore restore;
		for (const bool cli : {false, true}) {
			GlobalState::isNonInteractive.store(cli);
			checkReady(f);
			writeFile(f.inflight, "3 0\n");
			checkReady(f);
			writeFile(f.inflight, "0 2\n");
			checkRejected(f, "has pending kernel-level tasks");
			f.mounted(true);
			checkRejected(f, "or its partitions are mounted");
			f.mounted(false);
			writeFile(f.inflight, "0 0\n");
			checkReady(f);
			{
				Reservation reserved(f.device);
				checkRejected(f, "is still draining a cancelled write");
			}
			checkReady(f);
		}
	}

	/**
	 * @brief Checks parsing of synthetic in-flight I/O counters.
	 */
	void usbInflightParsing() {
		BusyFixture f;
		for (const auto* counters : {"0 0\n", "9 0\n", "", "invalid\n", "1\n", "1 invalid\n"}) {
			writeFile(f.inflight, counters);
			CHECK(!WriteValidation::isDeviceBusy(f.device, f.paths));
		}
		for (const auto* counters : {"0 1\n", "4 8\n", "  2\t3\n"}) {
			writeFile(f.inflight, counters);
			CHECK(WriteValidation::isDeviceBusy(f.device, f.paths));
		}
		fs::remove(f.inflight);
		CHECK(!WriteValidation::isDeviceBusy(f.device, f.paths));
	}
}
