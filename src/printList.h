// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PRINTLIST_H
#define PRINTLIST_H

#include <cstddef>
#include <string>
#include <vector>

/**
 * @brief Non-owning, read-only view over a list of display strings.
 *
 * Presents the same size()/empty()/operator[] interface as a
 * std::vector<std::string>, so render code can be written once and fed either
 * a plain list (sel == nullptr => identity view) or a subset of a larger list
 * selected by index (sel != nullptr), without copying any strings.
 *
 * Lifetime: the caller must keep *base and *sel alive for as long as the view
 * is used.
 */
struct ItemsView {
    const std::vector<std::string>* base = nullptr;
    const std::vector<size_t>*      sel  = nullptr; // nullptr => identity

    size_t size()  const { return sel ? sel->size() : base->size(); }
    bool   empty() const { return size() == 0; }

    const std::string& operator[](size_t i) const {
        return sel ? (*base)[(*sel)[i]] : (*base)[i];
    }
};

#endif // PRINTLIST_H
