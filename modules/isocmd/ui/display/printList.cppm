// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.display.printList;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

struct RefreshState;
class StringListView;



/// Holds the state and options required for printList.
struct PrintListContext {
	std::vector<std::string>& pendingIndices;
	bool& hasPendingProcess;
	std::size_t& currentPage;
	std::shared_ptr<RefreshState> state;

	bool clearFirst = true;
	bool showPendingIndices = true;
	bool showSourceIndices = true;

	std::size_t displayIndexOffset = 0;
};

/**
 * @brief Renders one page of an ISO, image, or mounted-ISO list to the terminal.
 *
 * @param items List entries to render.
 * @param listType Type of list being rendered.
 * @param listSubType Subtype used to configure row formatting.
 * @param ctx Rendering state and display options.
 */
void printList(const StringListView& items,
               const std::string& listType,
               const std::string& listSubType,
               const PrintListContext& ctx);

} // extern "C++"
