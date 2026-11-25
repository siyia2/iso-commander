// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import isocmd.write2usb.ui.textWidth;

std::size_t utf8Width(const std::string& s) {

	std::size_t w = 0;
	for (unsigned char c : s)
	    if ((c & 0xC0) != 0x80) ++w;
	return w;

}

std::size_t visibleWidth(const std::string& s) {

	std::size_t w = 0;
	for (std::size_t i = 0; i < s.size(); ) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
			i += 2;
			while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
			if (i < s.size()) ++i;
			continue;
		}
		if ((c & 0xC0) != 0x80) ++w;
		++i;
	}
	return w;

}

std::string clipToWidth(const std::string& s, std::size_t maxCols) {

	std::string out;
	out.reserve(s.size());
	std::size_t w = 0;
	for (std::size_t i = 0; i < s.size(); ) {
		if (static_cast<unsigned char>(s[i]) == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
			std::size_t j = i + 2;
			while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j;
			if (j < s.size()) ++j;
			out.append(s, i, j - i);
			i = j;
			continue;
		}
		std::size_t len = 1;
		while (i + len < s.size() &&
		    (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
		if (w >= maxCols) break;
		out.append(s, i, len);
		i += len;
		++w;
	}
	return out;

}

std::string truncateIsoName(const std::string& name, std::size_t maxCols) {

	if (utf8Width(name) <= maxCols) return name;

	std::string ext;
	std::string stem = name;
	if (name.size() >= 4) {
		const std::string tail = name.substr(name.size() - 4);
		if (tail[0] == '.' &&
		    std::tolower(static_cast<unsigned char>(tail[1])) == 'i' &&
		    std::tolower(static_cast<unsigned char>(tail[2])) == 's' &&
		    std::tolower(static_cast<unsigned char>(tail[3])) == 'o') {
			ext  = tail;
			stem = name.substr(0, name.size() - 4);
		}
	}

	constexpr std::size_t MIN_STEM = 4;
	const std::size_t overhead = 1 + utf8Width(ext);
	const std::size_t keep = std::max(MIN_STEM, maxCols > overhead ? maxCols - overhead : 0);

	std::string out;
	std::size_t cols = 0, i = 0;
	while (i < stem.size() && cols < keep) {
		std::size_t len = 1;
		while (i + len < stem.size() &&
		       (static_cast<unsigned char>(stem[i + len]) & 0xC0) == 0x80) ++len;
		out.append(stem, i, len);
		i += len;
		++cols;
	}
	if (i >= stem.size()) return name;

	out += "\u2026";
	out += ext;
	return out;

}
