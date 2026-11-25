// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.ui.display.display;
import isocmd.ui.display.stringListView;
import isocmd.util.sort;

namespace isocmd_tests {

	void sortingAndViews() {

		CHECK(naturalCompare("disk2.iso", "disk10.iso") < 0);
		CHECK(naturalCompare("DISK2.ISO", "disk2.iso") < 0); // original-case tie-break
		CHECK(naturalCompare("disk2.iso", "disk2.iso") == 0);
		CHECK(naturalCompare("disk99999999999999999999", "disk100000000000000000000") < 0);
		CHECK(naturalCompare("", "a") < 0);
		std::vector<std::string> files{"/a/disk10.iso", "/z/disk2.iso", "/b/disk1.iso"};
		displayConfig::toggleNamesOnly = false;
		sortFilesCaseInsensitive(files);
		CHECK(files == std::vector<std::string>({"/a/disk10.iso", "/b/disk1.iso", "/z/disk2.iso"}));
		displayConfig::toggleNamesOnly = true;
		sortFilesCaseInsensitive(files);
		CHECK(files == std::vector<std::string>({"/b/disk1.iso", "/z/disk2.iso", "/a/disk10.iso"}));
		displayConfig::toggleNamesOnly = false;
		std::vector<std::string> many;
		for (int n = 4096; n > 0; --n) many.push_back("disk" + std::to_string(n) + ".iso");
		sortFilesCaseInsensitive(many);
		for (int n = 1; n <= 4096; ++n) CHECK(many[n-1] == "disk" + std::to_string(n) + ".iso");
		const Indices subset{2,0};
		StringListView view(files, subset);
		CHECK(view.size() == 2 && &view[0] == &files[2]);
		CHECK(std::vector<std::string>(view.begin(), view.end()) == std::vector<std::string>({files[2], files[0]}));
		const Indices empty;
		CHECK(StringListView(files, empty).empty());
		CHECK(StringListView(files).size() == files.size());

	}

} // namespace isocmd_tests
