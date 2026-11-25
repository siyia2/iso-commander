// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.history;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- input/history ----

void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch,
				  const std::function<void()>& repaintHeader);

} // extern "C++"
