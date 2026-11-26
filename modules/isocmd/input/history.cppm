// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.history;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch,
				  const std::function<void()>& repaintHeader);

} // extern "C++"
