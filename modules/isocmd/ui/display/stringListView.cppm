// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.display.stringListView;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @brief Non-owning, zero-copy view over a std::vector<std::string>,
 * optionally restricted (and reordered) by a subset of indices.
 *
 * No string is ever copied. The referenced vectors must outlive the view;
 * consumers (e.g. printList) must not retain it past the call.
 */
class StringListView {
public:
	// Implicit on purpose: existing printList(std::vector<std::string>, ...)
	// call sites keep compiling unchanged.
	StringListView(const std::vector<std::string>& base) noexcept
	    : base_(&base) {}

	StringListView(const std::vector<std::string>& base,
	               const std::vector<std::size_t>& subset) noexcept
	    : base_(&base), subset_(&subset) {}

	std::size_t size()  const noexcept {

		return subset_ ? subset_->size() : base_->size();

	}

	bool   empty() const noexcept {

		return size() == 0;

	}

	const std::string& operator[](std::size_t i) const noexcept {

		return subset_ ? (*base_)[(*subset_)[i]] : (*base_)[i];

	}

private:
	const std::vector<std::string>* base_;
	const std::vector<std::size_t>*      subset_ = nullptr;
};

} // extern "C++"
