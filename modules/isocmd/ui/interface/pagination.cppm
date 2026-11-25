// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.interface.pagination;

import std;

export extern "C++" {

/**
 * @brief Renders a paginated list and captures one line of user input.
 */
std::string handlePaginatedDisplay(
	const std::vector<std::string>& entries,
	const std::string& promptPrefix,
	const std::string& promptSuffix,
	const std::function<void()>& setupEnvironmentFn,
	bool& isPageTurn,
	std::size_t& currentPage,
	bool disableTabCompletion = false
);

} // extern "C++"
