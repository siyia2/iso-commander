// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.settings.settings;
import isocmd.app.main;
import isocmd.core.uiState;
import isocmd.util.stringManipulation;

namespace isocmd_tests {

	static void validateConfigDefaults() {

		CHECK(isNum("0", 0, 10) && isNum("10", 0, 10));
		for (const std::string bad : {"", "00", "01", "-1", "+1", "1.0", " 1", "1 ", "11", "999999999999999999999"})
		    CHECK(!isNum(bad, 0, 10));
		CHECK(trim(" \t test \t") == "test");
		std::unordered_set<std::string> keys;
		for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
			CHECK(keys.insert(entry.key).second);
			CHECK(entry.validate(entry.defaultValue));
		}
		const auto* checksum = findConfigEntry("verify_checksum");
		CHECK(checksum && checksum->defaultValue == "off");
		CHECK(checksum->validate("on") && checksum->validate("off"));
		CHECK(!checksum->validate("yes") && !checksum->validate(""));
		const auto* buffered = findConfigEntry("buffered_writes");
		CHECK(buffered && buffered->defaultValue == "off");
		CHECK(buffered->validate("on") && buffered->validate("off"));
		CHECK(!buffered->validate("yes") && !buffered->validate(""));

	}

	static void verifyUsbSettingsSection() {

		std::string section;
		std::vector<std::string> usbKeys;
		for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
			if (!entry.section.empty()) section = entry.section;
			if (section == "Write2USB Settings") usbKeys.push_back(entry.key);
			if (entry.key == "write2usb_list") CHECK(section == "Display Modes");
		}
		CHECK(usbKeys == std::vector<std::string>{"verify_checksum", "buffered_writes"});

	}

	static void verifyConfigRepair(const std::string& file) {

		{ std::ofstream out(file); out << "pagination = 000\nauto_update = on\ntheme = invalid\n"; }
		syncCache(file);
		CHECK(ConfigCaches::g_configCache.at("pagination") == "25");
		CHECK(ConfigCaches::g_configCache.at("auto_update") == "on");
		CHECK(ConfigCaches::g_configCache.at("theme") == "original");
		CHECK(ConfigCaches::g_configCache.at("verify_checksum") == "off");
		CHECK(!write2usbChecksumEnabled());
		CHECK(!write2usbBufferedEnabled());
		for (const auto& e : CONFIG_ORDERED_DEFAULTS) CHECK(e.validate(ConfigCaches::g_configCache.at(e.key)));
		CHECK(paginationSet(file));
		CHECK(GlobalState::ITEMS_PER_PAGE == 25);

	}

	static void verifyConfigOrder(const std::string& file) {

		std::ifstream in(file);
		std::string line;
		std::vector<std::string> diskKeys;
		while (std::getline(in, line)) {
			if (!line.empty() && line[0] != '#' && line.find('=') != std::string::npos)
			    diskKeys.push_back(trim(line.substr(0, line.find('='))));
		}
		CHECK(diskKeys.size() == CONFIG_ORDERED_DEFAULTS.size());
		for (std::size_t i = 0; i < diskKeys.size(); ++i) CHECK(diskKeys[i] == CONFIG_ORDERED_DEFAULTS[i].key);
		in.close();

	}

	static void verifyUsbSettingsPersistence(const std::string& file) {

		ConfigCaches::g_configCache["verify_checksum"] = "on";
		ConfigCaches::g_configCache["buffered_writes"] = "on";
		CHECK(write2usbChecksumEnabled());
		CHECK(write2usbBufferedEnabled());
		CHECK(writeConfig(file, ConfigCaches::g_configCache));
		ConfigCaches::g_configCache.clear();
		syncCache(file);
		CHECK(write2usbChecksumEnabled());
		CHECK(write2usbBufferedEnabled());
		ConfigCaches::g_configCache["verify_checksum"] = "off";
		CHECK(!write2usbChecksumEnabled());
		ConfigCaches::g_configCache["verify_checksum"] = "invalid";
		ConfigCaches::g_configCache["buffered_writes"] = "invalid";
		CHECK(writeConfig(file, ConfigCaches::g_configCache));
		ConfigCaches::g_configCache.clear();
		syncCache(file);
		CHECK(ConfigCaches::g_configCache.at("verify_checksum") == "off");
		CHECK(!write2usbChecksumEnabled());
		CHECK(ConfigCaches::g_configCache.at("buffered_writes") == "off");
		CHECK(!write2usbBufferedEnabled());
		ConfigCaches::g_configCache.clear();
		ConfigCaches::g_cachedPath.clear();
		CHECK(!write2usbChecksumEnabled());
		CHECK(!write2usbBufferedEnabled());

	}

	void configuration() {

		validateConfigDefaults();
		verifyUsbSettingsSection();
		TempDir tmp;
		const auto file = (tmp.path / "config").string();
		verifyConfigRepair(file);
		verifyConfigOrder(file);
		CHECK(readFile(file).find("# --- Write2USB Settings ---\n") != std::string::npos);
		verifyUsbSettingsPersistence(file);

	}

} // namespace isocmd_tests
