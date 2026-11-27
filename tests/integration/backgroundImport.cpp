// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Exercises background import, deduplication, cancellation and restart.
 */

#include "../common/testHarness.h"

import isocmd.core.paths;
import isocmd.core.sharedRefreshState;
import isocmd.database.databaseOps;
import isocmd.core.cancellation;

namespace isocmd_tests {

	void backgroundImport() {

		const char* home = std::getenv("HOME"), *sandbox = std::getenv("ISOCMD_TEST_SANDBOX");
		CHECK(home && sandbox && fs::path(home) == fs::path(sandbox)/"home");
		CHECK(fs::path(GlobalState::databaseFilePath).parent_path() == fs::path(home)/".local/share/isocmd/database");
		CHECK(fs::path(GlobalState::historyFilePath).parent_path() == fs::path(GlobalState::databaseFilePath).parent_path());
		CHECK(!fs::exists(GlobalState::databaseFilePath));
		fs::create_directories(GlobalState::databaseDirectory);
		TempDir tmp; fs::create_directories(tmp.path/"nested");
		const auto a = tmp.path/"a.iso", b = tmp.path/"nested/b.ISO", c = tmp.path/"nested/c.iso";
		writeFile(a, "a"); writeFile(b, "b"); writeFile(tmp.path/"ignore.txt", "x");
		fs::create_directory_symlink(tmp.path/"nested", tmp.path/"alias");
		writeFile(GlobalState::historyFilePath, tmp.path.string()+";relative;"+tmp.path.string()+"\n"+tmp.path.string()+"\n");
		auto run = [](bool stop) {

			auto state = std::make_shared<RefreshState>(); state->isImportRunning = true; state->stopImport = stop;
			std::jthread worker([state]{ backgroundDatabaseImport(state); });
			// signalImportDone uses printMutex, not importMutex. Polling the atomic
			// avoids relying on a notification arriving before the wait begins.
			const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
			while (state->isImportRunning && std::chrono::steady_clock::now()<deadline)
			    std::this_thread::sleep_for(std::chrono::milliseconds(10));
			CHECK(!state->isImportRunning); worker.join(); CHECK(state->activeWorkers == 0);

		};
		run(false);
		auto verify = [&](std::vector<std::string> expected) { std::vector<std::string> got; loadFromDatabase(got); std::sort(got.begin(), got.end()); std::sort(expected.begin(), expected.end()); CHECK(got == expected); };
		verify({a.string(), b.string()}); run(false); verify({a.string(), b.string()});
		writeFile(c, "c"); run(true); verify({a.string(), b.string()});
		run(false); verify({a.string(), b.string(), c.string()});
		fs::remove(GlobalState::historyFilePath); run(false); verify({a.string(), b.string(), c.string()});
		fs::remove(GlobalState::databaseFilePath);

	}
}
