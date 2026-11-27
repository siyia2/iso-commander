// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks compact mount and unmount results without requiring root or mounted images.
 */

#include "../common/testHarness.h"

import isocmd.ui.display.display;
import isocmd.ui.info.verbose;

namespace isocmd_tests {

	namespace {

		/// Removes theme SGR sequences while retaining UTF-8 status symbols and paths.
		std::string plainResult(std::string_view line) {

			std::string text;
			for (std::size_t i = 0; i < line.size(); ++i) {
				if (line[i] == '\033' && i + 1 < line.size() && line[i+1] == '[') {
					const auto end = line.find('m', i+2);
					CHECK(end != std::string_view::npos);
					i = end;
				} else {
					text += line[i];
				}
			}
			return text;

		}

		/// Isolates result records and restores display preferences on every exit path.
		struct VerboseResultGuard {
			bool namesOnly = displayConfig::toggleNamesOnly;
			bool fullMount = displayConfig::toggleFullListMount;
			bool fullUmount = displayConfig::toggleFullListUmount;

			VerboseResultGuard() { clearGlobalVerboseSets(); }
			~VerboseResultGuard() {
				displayConfig::toggleNamesOnly = namesOnly;
				displayConfig::toggleFullListMount = fullMount;
				displayConfig::toggleFullListUmount = fullUmount;
				clearGlobalVerboseSets();
			}
		};

		/// Collects the distinct rendered lines for a mount outcome category.
		std::vector<std::string> mountLines(MountRecKind kind) {

			std::vector<std::string> lines;
			forEachMountLine(kind, [&](std::string_view line) {
				lines.push_back(plainResult(formatVerboseLine("mount", line)));
			});
			return lines;

		}

		/// Collects the distinct rendered lines for successful or failed unmounts.
		std::vector<std::string> umountLines(bool completed) {

			std::vector<std::string> lines;
			forEachUmountLine(completed, [&](std::string_view line) {
				lines.push_back(plainResult(formatVerboseLine("umount", line)));
			});
			return lines;

		}

		/// Verifies all reported filesystem types, case-insensitive ordering and deduplication.
		void checkMountSuccessLines() {

			verboseSets.mountRecords = {
				{"/images/zeta.iso", "udf", MountRecKind::Completed},
				{"/images/Beta.iso", "hfsplus", MountRecKind::Completed},
				{"/images/alpha disk.iso", "iso9660", MountRecKind::Completed},
				{"/images/classic.iso", "hfs", MountRecKind::Completed},
				{"/images/unknown.iso", "unknown", MountRecKind::Completed},
				{"/images/zeta.iso", "udf", MountRecKind::Completed},
				{"/images/existing.iso", nullptr, MountRecKind::Skipped}
			};
			const auto lines = mountLines(MountRecKind::Completed);
			CHECK(lines.size() == 5);
			CHECK(lines[0].starts_with("✓ mount: /images/alpha disk.iso → /mnt/ISOs/alpha disk~") && lines[0].ends_with("  {iso9660}"));
			CHECK(lines[1].starts_with("✓ mount: /images/Beta.iso → ") && lines[1].ends_with("  {hfsplus}"));
			CHECK(lines[2].ends_with("  {hfs}"));
			CHECK(lines[3].ends_with("  {unknown}"));
			CHECK(lines[4].ends_with("  {udf}"));
			const auto skipped = mountLines(MountRecKind::Skipped);
			CHECK(skipped.size() == 1 && skipped[0].starts_with("– mount: /images/existing.iso → /mnt/ISOs/existing~"));
			displayConfig::toggleNamesOnly = true;
			CHECK(mountLines(MountRecKind::Completed).front().starts_with("✓ mount: alpha disk.iso → "));
			displayConfig::toggleNamesOnly = false;

		}

		/// Checks that every mount error code is emitted verbatim inside braces.
		void checkMountErrorLines() {

			for (const char* code : {"badFS", "cxl", "missingISO", "mkdirFailed", "needsRoot", "needsHFS", "needsHFSPlus"}) {
				verboseSets.mountRecords.push_back({"/images/broken.iso", code, MountRecKind::Failed});
			}
			const auto lines = mountLines(MountRecKind::Failed);
			CHECK(lines.size() == 7);
			for (std::string_view code : {"badFS", "cxl", "missingISO", "mkdirFailed", "needsRoot", "needsHFS", "needsHFSPlus"}) {
				const auto expected = "✗ mount: /images/broken.iso  {" + std::string(code) + "}";
				CHECK(std::find(lines.begin(), lines.end(), expected) != lines.end());
			}

		}

		/// Checks compact unmount results with full paths and the names-only display.
		void checkUmountLines() {

			verboseSets.umountRecords = {
				{"/mnt/ISOs/alpha~abcde", "success", true},
				{"/mnt/ISOs/beta~abcde", "error", false},
				{"/mnt/ISOs/cancel~abcde", "cancel", false},
				{"/mnt/ISOs/root~abcde", "root_error", false}
			};
			CHECK(umountLines(true) == std::vector<std::string>{"✓ umount: /mnt/ISOs/alpha~abcde"});
			CHECK(umountLines(false) == (std::vector<std::string>{
				"✗ umount: /mnt/ISOs/beta~abcde  {notAnISO}",
				"✗ umount: /mnt/ISOs/cancel~abcde  {cxl}",
				"✗ umount: /mnt/ISOs/root~abcde  {needsRoot}"
			}));
			displayConfig::toggleFullListUmount = false;
			CHECK(umountLines(true) == std::vector<std::string>{"✓ umount: alpha"});

		}

	} // namespace

	/// Runs the pure rendering checks without performing any mount operations.
	void compactVerboseResults() {

		VerboseResultGuard guard;
		displayConfig::toggleNamesOnly = false;
		displayConfig::toggleFullListMount = true;
		displayConfig::toggleFullListUmount = true;
		checkMountSuccessLines();
		checkMountErrorLines();
		checkUmountLines();

	}

} // namespace isocmd_tests
