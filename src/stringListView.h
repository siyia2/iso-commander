// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LISTVIEW_H
#define LISTVIEW_H

#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

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
                   const std::vector<size_t>& subset) noexcept
        : base_(&base), subset_(&subset) {}

    size_t size()  const noexcept { return subset_ ? subset_->size() : base_->size(); }
    bool   empty() const noexcept { return size() == 0; }

    const std::string& operator[](size_t i) const noexcept {
        return subset_ ? (*base_)[(*subset_)[i]] : (*base_)[i];
    }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::string;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const std::string*;
        using reference         = const std::string&;

        const_iterator(const StringListView* v, size_t i) noexcept : v_(v), i_(i) {}
        reference operator*()  const noexcept { return (*v_)[i_]; }
        pointer   operator->() const noexcept { return &(*v_)[i_]; }
        const_iterator& operator++() noexcept { ++i_; return *this; }
        const_iterator  operator++(int) noexcept { auto t = *this; ++i_; return t; }
        bool operator==(const const_iterator& o) const noexcept { return i_ == o.i_; }
        bool operator!=(const const_iterator& o) const noexcept { return i_ != o.i_; }
    private:
        const StringListView* v_;
        size_t i_;
    };

    const_iterator begin() const noexcept { return {this, 0}; }
    const_iterator end()   const noexcept { return {this, size()}; }

private:
    const std::vector<std::string>* base_;
    const std::vector<size_t>*      subset_ = nullptr;
};

#endif // LISTVIEW_H
