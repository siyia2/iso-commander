// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks real ISO mounting and unmounting in an isolated mount namespace.
 */

#include <sys/stat.h>
#include "../common/testHarness.h"

import isocmd.ops.mount;
import isocmd.ops.umount;

namespace isocmd_tests {

	/**
	 * @brief Checks a real mount and unmount using the private namespace fixture.
	 */
	int mountTest() {

		try {
			const char* env = std::getenv("ISOCMD_TEST_SANDBOX"); CHECK(env);
			const auto sandbox = fs::canonical(env), mountRoot = sandbox/"mnt";
			CHECK(geteuid() == 0 && fs::current_path() == sandbox);
			// The shell has bind-mounted a private, empty directory over /mnt
			// inside a fresh mount namespace. Fail closed if that setup is absent.
			struct stat real{}, isolated{}, ns{}, host{};
			CHECK(stat("/mnt", &real) == 0 && stat(mountRoot.c_str(), &isolated) == 0);
			CHECK(real.st_dev == isolated.st_dev && real.st_ino == isolated.st_ino && fs::is_empty("/mnt"));
			CHECK(stat("/proc/self/ns/mnt", &ns) == 0 && stat("/proc/1/ns/mnt", &host) == 0 && ns.st_ino != host.st_ino);
			const auto iso = sandbox/"mount.iso";
			CHECK(fs::is_regular_file(iso) && !fs::is_symlink(iso));
			std::atomic<std::size_t> done{0}, failed{0};
			mountIsoFiles({iso.string()}, &done, &failed, true);
			CHECK(done == 1 && failed == 0);
			std::vector<fs::path> mounts;
			for (const auto& entry:fs::directory_iterator("/mnt/ISOs")) mounts.push_back(entry.path());
			CHECK(mounts.size() == 1 && fs::is_directory(mounts[0]));
			// Rock Ridge preserves the lowercase filename from the generated fixture.
			CHECK(readFile(mounts[0]/"marker.txt") == "isocmd mount test\n");
			unmountISO({mounts[0].string()}, &done, &failed, true);
			CHECK(done == 2 && failed == 0 && !fs::exists(mounts[0]));
			std::cout << "PASS: ISO mount, read and unmount in private namespace\n";
			return 0;
		} catch (const std::exception& e) { std::cerr << "FAIL: mount integration: " << e.what() << '\n'; return 1; }

	}
}
