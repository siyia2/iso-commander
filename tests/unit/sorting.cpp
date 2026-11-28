// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks natural ordering, sort modes and list views.
 */

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
		// Case and zero tie-breaks apply immediately, before later characters.
		CHECK(naturalCompare("A9", "a1") < 0);
		CHECK(naturalCompare("disk2z", "disk02a") < 0);
		CHECK(naturalCompare("0", "00") < 0);
		CHECK(naturalCompare("000", "1") < 0);
		CHECK(naturalCompare("disk09", "disk10") < 0);
		CHECK(naturalCompare("disk99", "disk100") < 0);
		CHECK(naturalCompare("disk10", "disk11") < 0);
		CHECK(naturalCompare("disk2part9", "disk2part10") < 0);
		CHECK(naturalCompare("disk0002", "disk0002") == 0);
		CHECK(naturalCompare("disk2", "disk2.iso") < 0);
		CHECK(naturalCompare("é2.iso", "é10.iso") < 0);
		CHECK(naturalCompare("disk2a", "disk2A") > 0);
		CHECK(naturalCompare("disk9", "disk1a") > 0);
		CHECK(naturalCompare("disk99999999999999999998", "disk99999999999999999999") < 0);
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
		const Indices subset{2, 0};
		StringListView view(files, subset);
		CHECK(view.size() == 2 && !view.empty());
		CHECK(&view[0] == &files[2] && &view[1] == &files[0]);
		const Indices empty;
		CHECK(StringListView(files, empty).empty());
		const StringListView fullView(files);
		CHECK(fullView.size() == files.size() && !fullView.empty());
		for (std::size_t i = 0; i < fullView.size(); ++i) CHECK(&fullView[i] == &files[i]);
		const std::vector<std::string> emptyFiles;
		CHECK(StringListView(emptyFiles).empty());

	}

} // namespace isocmd_tests
