// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>

import std;
import isocmd.core.uiState;
import isocmd.input.readline;
import isocmd.select.selectShared;

ReadlineKeybindingGuard::ReadlineKeybindingGuard() {
    setup_custom_keybindingsForSelect();
}

ReadlineKeybindingGuard::~ReadlineKeybindingGuard() {
    reset_custom_keybindingsForSelect();
    RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
}

void ReadlineKeybindingGuard::restore() {
    setup_custom_keybindingsForSelect();
}

IsoListStateGuard::~IsoListStateGuard() {
    if (!isUnmountCheck_()) {
        isAtISOList_.store(wasAtList_, std::memory_order_release);
    }

    GlobalState::g_filteringIndicator.store(false, std::memory_order_release);
}

namespace select_shared {

namespace term {

void rewindToPromptRow() {
    std::cout << "\033[1A\r\033[K";
}

} // namespace term

namespace pending {

bool parsePositiveInt(std::string_view view, int &value) {
    if (view.empty() || view.front() == '0') {
        return false;
    }

    const bool allDigits = std::all_of(view.begin(), view.end(),
                                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
    if (!allDigits) {
        return false;
    }

    const auto [endPtr, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    return error == std::errc{} && endPtr == view.data() + view.size();
}

bool parseIndexToken(std::string_view token, IndexToken &out) {
    out = {};

    const bool hasSourceSuffix = !token.empty() && token.back() == '^';
    if (hasSourceSuffix) {
        token.remove_suffix(1);
    }

    if (token.empty()) {
        return false;
    }

    const size_t firstDash = token.find('-');
    const bool hasMultipleDashes = firstDash != std::string_view::npos &&
                                   token.find('-', firstDash + 1) != std::string_view::npos;
    if (hasMultipleDashes) {
        return false;
    }

    int lo = 0;
    int hi = 0;

    if (firstDash == std::string_view::npos) {
        if (!parsePositiveInt(token, lo)) {
            return false;
        }
        hi = lo;
    } else {
        const std::string_view left = token.substr(0, firstDash);
        const std::string_view right = token.substr(firstDash + 1);
        if (!parsePositiveInt(left, lo) || !parsePositiveInt(right, hi)) {
            return false;
        }
    }

    if (lo > hi) {
        std::swap(lo, hi);
    }

    out.source = hasSourceSuffix;
    out.lo = static_cast<size_t>(lo);
    out.hi = static_cast<size_t>(hi);
    return true;
}

std::vector<size_t> resolveIndices(std::vector<std::string> &paths,
                                   const std::vector<std::string> &source, bool prune) {
    if (paths.empty())
        return {};
    std::unordered_map<std::string_view, size_t> indexOf;
    indexOf.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        indexOf.emplace(source[i], i + 1); // first occurrence wins

    std::vector<size_t> idx;
    idx.reserve(paths.size());

    if (prune) {
        std::vector<std::string> kept;
        kept.reserve(paths.size());
        for (auto &p : paths) {
            const auto it = indexOf.find(p);
            if (it == indexOf.end())
                continue;
            idx.push_back(it->second);
            kept.push_back(std::move(p));
        }
        paths = std::move(kept);
    } else {
        for (const auto &p : paths) {
            const auto it = indexOf.find(p);
            if (it != indexOf.end())
                idx.push_back(it->second);
        }
    }

    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    return idx;
}

std::vector<std::string> compressRuns(const std::vector<size_t> &idx) {
    std::vector<std::string> out;
    for (size_t i = 0; i < idx.size();) {
        size_t j = i;
        while (j + 1 < idx.size() && idx[j + 1] == idx[j] + 1)
            ++j;
        out.push_back(i == j ? std::to_string(idx[i])
                             : std::to_string(idx[i]) + "-" + std::to_string(idx[j]));
        i = j + 1;
    }
    return out;
}

std::string joinTokens(const std::vector<std::string> &tokens) {
    std::string s;
    for (const auto &t : tokens) {
        if (!s.empty())
            s += ' ';
        s += t;
    }
    return s;
}

bool stagePending(const std::string &inputString, std::vector<std::string> &paths,
                  bool &hasPendingProcess, bool &needsClrScrn,
                  const std::vector<std::string> &viewList,
                  const std::vector<std::string> &sourceList) {
    const size_t semi = inputString.find(';');
    if (semi == std::string::npos || inputString.find('/') != std::string::npos)
        return false;

    std::istringstream iss(inputString.substr(0, semi));
    std::unordered_set<std::string> seen(paths.begin(), paths.end());

    std::string tok;
    while (iss >> tok) {
        IndexToken t;
        if (!parseIndexToken(tok, t))
            continue;

        const std::vector<std::string> &list = t.source ? sourceList : viewList;

        // Match tokenizeInput(): reject the entire token if either
        // endpoint exceeds the available list size.
        if (t.lo > list.size() || t.hi > list.size())
            continue;

        for (size_t n = t.lo;; ++n) {
            const std::string &path = list[n - 1];
            if (seen.insert(path).second)
                paths.push_back(path);

            if (n == t.hi)
                break;
        }
    }

    if (paths.empty())
        return false;
    hasPendingProcess = true;
    needsClrScrn = true;
    return true;
}

} // namespace pending

namespace select_common {

bool isFilterCommand(const std::string &input) {
    return !input.empty() && input[0] == '/';
}

} // namespace select_common

} // namespace select_shared
