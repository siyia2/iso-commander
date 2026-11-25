// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.core.paths;
import isocmd.database.databaseOps;

namespace isocmd_tests {
	void database() {

		// The runner sets HOME before process startup. Refuse to touch any other DB.
		const char* home = std::getenv("HOME");
		const char* sandbox = std::getenv("ISOCMD_TEST_SANDBOX");
		CHECK(home && sandbox && fs::path(home) == fs::path(sandbox) / "home");
		CHECK(fs::path(GlobalState::databaseFilePath).parent_path() == fs::path(home) / ".local/share/isocmd/database");
		CHECK(!fs::exists(GlobalState::databaseFilePath));
		TempDir tmp;
		const auto a = (tmp.path / "a.iso").string(), b = (tmp.path / "β.iso").string();
		writeFile(a, "a"); writeFile(b, "b");
		bool found = false;
		CHECK(saveToDatabase({a,a,b}, &found) && found);
		CHECK(!saveToDatabase({b,a}, &found) && !found);
		std::vector<std::string> loaded;
		loadFromDatabase(loaded); CHECK(loaded == std::vector<std::string>({a,b}));
		// Concurrent writers with overlapping paths: each unique path persists once.
		std::vector<std::string> expected{a,b};
		for (int i=0; i<32; ++i) {
			expected.push_back((tmp.path / (std::to_string(i)+".iso")).string());
			writeFile(expected.back(), "x");
		}
		std::vector<std::jthread> workers;
		for (int w=0; w<4; ++w) workers.emplace_back([&,w] {

			for (int j=0; j<8; ++j) saveToDatabase({a,expected[2+w*8+j]}, nullptr);

		});
		workers.clear();
		loadFromDatabase(loaded);
		std::sort(loaded.begin(),loaded.end()); std::sort(expected.begin(),expected.end());
		CHECK(loaded == expected);
		fs::remove(a); fs::remove(b);
		removeNonExistentPathsFromDatabase(loaded);
		std::erase(expected,a); std::erase(expected,b);
		std::sort(loaded.begin(),loaded.end()); CHECK(loaded == expected);
		std::vector<std::string> persisted;
		loadFromDatabase(persisted); std::sort(persisted.begin(),persisted.end()); CHECK(persisted == expected);
		fs::remove(GlobalState::databaseFilePath);

	}

}
