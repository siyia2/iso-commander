// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks selection parsing, ranges, duplicates and invalid indices.
 */

#include "../common/testHarness.h"

import isocmd.input.tokenize;
import isocmd.ui.info.verbose;

namespace isocmd_tests {

	void selection() {

		const std::vector<std::string> files(10, "image.iso");
		std::unordered_set<int> selected;
		verboseSets.uniqueErrorTokenMessages.clear();
		tokenizeInput("1 3-5 8-6 3 10", files, selected);
		CHECK(selected == std::unordered_set<int>({1, 3, 4, 5, 6, 7, 8, 10}));
		CHECK(verboseSets.uniqueErrorTokenMessages.empty());
		selected.clear();
		tokenizeInput("0 01 11 999999999999999 1-11 1-2-3 -2 2x 4", files, selected);
		CHECK(selected == std::unordered_set<int>({4}));
		const auto& errors = verboseSets.uniqueErrorTokenMessages;
		CHECK(errors.size() == 3);
		for (std::string_view label : {"Invalid inputs", "Invalid indices", "Invalid range"})
		    CHECK(std::any_of(errors.begin(), errors.end(), [&](const auto& e) { return e.find(label) != std::string::npos; }));
		verboseSets.uniqueErrorTokenMessages.clear();
		selected.clear();
		tokenizeInput("1", {}, selected);
		CHECK(selected.empty());
		CHECK(verboseSets.uniqueErrorTokenMessages.size() == 1);
		verboseSets.uniqueErrorTokenMessages.clear();

	}

} // namespace isocmd_tests
