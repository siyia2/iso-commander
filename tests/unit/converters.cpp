// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.ops.convert;
import isocmd.core.cancellation;

namespace isocmd_tests {

    void conversion() {
        TempDir tmp;
        const auto input = (tmp.path / "image.img").string();
        const auto output = (tmp.path / "converted.iso").string();
        const auto write = [&](const std::string& data) {
            std::ofstream out(input, std::ios::binary | std::ios::trunc);
            out.write(data.data(), data.size()); CHECK(out.good());
        };
        const auto read = [&] {
            std::ifstream in(output, std::ios::binary);
            CHECK(in.good());
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        std::string raw(2352 * 2, '\0');
        raw[15] = 1; raw[2352 + 15] = 2;
        std::string first(2048, 'A'), second(2048, 'B');
        raw.replace(16, 2048, first);
        raw.replace(2352 + 24, 2048, second);
        write(raw);
        std::atomic<std::size_t> completed{0};
        CHECK(convertCcdToIso(input, output, &completed));
        CHECK(read() == first + second && completed.load() == 4096);
        raw[15] = 3; write(raw); completed = 0;
        CHECK(!convertCcdToIso(input, output, &completed));
        CHECK(completed.load() == 0);
        fs::remove(output);
        GlobalState::g_operationCancelled = true;
        const bool cancelled = convertCcdToIso(input, output, &completed);
        GlobalState::g_operationCancelled = false;
        CHECK(!cancelled && !fs::exists(output));
        CHECK(!convertCcdToIso(input + ".missing", output, nullptr));
        std::string header(307200, '\0');
        const std::string payload(1024 * 1024 + 13, 'N');
        write(header + payload); completed = 0;
        CHECK(convertNrgToIso(input, output, &completed));
        CHECK(read() == payload && completed.load() == payload.size());
        fs::remove(output);
        header.replace(32768, 8, "\001CD001\001\000", 8);
        write(header + payload);
        CHECK(!convertNrgToIso(input, output, nullptr));
        CHECK(!fs::exists(output));
    }

} // namespace isocmd_tests
