// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.settings.settings;
import isocmd.app.main;
import isocmd.core.uiState;
import isocmd.util.stringManipulation;

namespace isocmd_tests {

	void configuration() {

		CHECK(isNum("0", 0, 10) && isNum("10", 0, 10));
		for (const std::string bad : {"", "00", "01", "-1", "+1", "1.0", " 1", "1 ", "11", "999999999999999999999"})
		    CHECK(!isNum(bad, 0, 10));
		CHECK(trim(" \t test \t") == "test");
		std::unordered_set<std::string> keys;
		for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
			CHECK(keys.insert(entry.key).second);
			CHECK(entry.validate(entry.defaultValue));
		}
		TempDir tmp;
		const auto file = (tmp.path / "config").string();
		{ std::ofstream out(file); out << "pagination = 000\nauto_update = on\ntheme = invalid\n"; }
		syncCache(file);
		CHECK(ConfigCaches::g_configCache.at("pagination") == "25");
		CHECK(ConfigCaches::g_configCache.at("auto_update") == "on");
		CHECK(ConfigCaches::g_configCache.at("theme") == "original");
		for (const auto& e : CONFIG_ORDERED_DEFAULTS) CHECK(e.validate(ConfigCaches::g_configCache.at(e.key)));
		CHECK(paginationSet(file));
		CHECK(GlobalState::ITEMS_PER_PAGE == 25);
		std::ifstream in(file);
		std::string line;
		std::vector<std::string> diskKeys;
		while (std::getline(in, line)) {
			if (!line.empty() && line[0] != '#' && line.find('=') != std::string::npos)
			    diskKeys.push_back(trim(line.substr(0, line.find('='))));
		}
		CHECK(diskKeys.size() == CONFIG_ORDERED_DEFAULTS.size());
		for (std::size_t i = 0; i < diskKeys.size(); ++i) CHECK(diskKeys[i] == CONFIG_ORDERED_DEFAULTS[i].key);
		ConfigCaches::g_configCache.clear();
		ConfigCaches::g_cachedPath.clear();

	}

} // namespace isocmd_tests
