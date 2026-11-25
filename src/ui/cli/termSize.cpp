// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/ioctl.h>
#include <unistd.h>

import std;
import isocmd.ui.cli.termSize;

// Projetc Headers

TermSize terminalSize() {

	static std::atomic<std::uint64_t> lastGood{0};

	struct winsize ws{};

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_col > 0 && ws.ws_row > 0) {
		lastGood.store(
		    (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row},
		    std::memory_order_relaxed);

		return {ws.ws_col, ws.ws_row};
	}

	const std::uint64_t v = lastGood.load(std::memory_order_relaxed);

	return {
		static_cast<std::size_t>(v >> 32),
		static_cast<std::size_t>(v & 0xFFFFFFFFu)
	};

}

std::size_t visibleWidthFrom(
    const std::string& s,
    std::size_t from) {

	std::size_t w = 0;

	for (std::size_t i = from; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);

		if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
			i += 2;

			while (i < s.size() &&
			       !(s[i] >= 0x40 && s[i] <= 0x7E)) {
				++i;
			}

			if (i < s.size()) {
				++i;
			}

			continue;
		}

		if ((c & 0xC0) != 0x80) {
			++w;
		}

		++i;
	}

	return w;

}

std::size_t utf8Cols(std::string_view s) noexcept {

	std::size_t w = 0;

	for (unsigned char c : s) {
		if ((c & 0xC0) != 0x80) {
			++w;
		}
	}

	return w;

}

std::string_view tailColumns(
    std::string_view s,
    std::size_t cols) noexcept {

	if (cols == 0) {
		return {};
	}

	std::size_t seen = 0;

	for (std::size_t i = s.size(); i > 0;) {
		--i;

		if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 &&
		    ++seen == cols) {
			return s.substr(i);
		}
	}

	return s;

}

void fitRowToWidth(
    std::string& out,
    std::size_t from,
    std::size_t maxCols) {

	if (maxCols < 2) {
		return;
	}

	std::size_t w = 0;
	std::size_t cutAt = std::string::npos;

	for (std::size_t i = from; i < out.size();) {
		const unsigned char c = static_cast<unsigned char>(out[i]);

		if (c == 0x1B && i + 1 < out.size() && out[i + 1] == '[') {
			i += 2;

			while (i < out.size() &&
			       !(out[i] >= 0x40 && out[i] <= 0x7E)) {
				++i;
			}

			if (i < out.size()) {
				++i;
			}

			continue;
		}

		if ((c & 0xC0) != 0x80) {
			if (w == maxCols - 1) {
				cutAt = i;
			}

			++w;
		}

		++i;
	}

	if (w > maxCols && cutAt != std::string::npos) {
		out.resize(cutAt);
		out.append("\u2026");
	}

}

std::string promptHeaderForRepaint(const std::string& prompt) {

	const std::size_t nl = prompt.rfind('\n');

	if (nl == std::string::npos) {
		return {};
	}

	std::string out;
	out.reserve(nl);

	for (std::size_t i = 0; i < nl; ++i) {
		const char c = prompt[i];

		if (c != '\001' && c != '\002') {
			out.push_back(c);
		}
	}

	return out;

}
