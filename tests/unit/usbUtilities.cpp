// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.write2usb.util.isoDetect;
import isocmd.write2usb.util.alignedBuffer;
import isocmd.write2usb.util.writeTypes;

namespace isocmd_tests {

void isoDetection() {
    TempDir tmp;
    const auto path = tmp.path / "fixture.iso";
    const auto check = [&](const std::string& bytes, bool optical, bool windows) {
        { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(bytes.data(), bytes.size()); CHECK(out.good()); }
        ReadFd f(path);
        CHECK(iso_detect::isValidIso9660(f.fd) == optical);
        CHECK(iso_detect::isWindowsIsoInitialCheck(f.fd, static_cast<off_t>(bytes.size())) == windows);
    };
    check("tiny", false, false);
    std::string bytes(65536, '\0');
    check(bytes, false, false);
    bytes.replace(32768, 6, "\001CD001", 6);
    check(bytes, true, false);
    bytes.replace(40000, 8, "BOOT.WIM");
    check(bytes, true, false);
    bytes.replace(41000, 11, "BootMgr.EFI");
    check(bytes, true, true);
    bytes.replace(32768, 6, "\002CD001", 6);
    check(bytes, false, false);
    bytes.replace(32768, 6, "\000NSR03", 6);
    check(bytes, true, true);
    std::string udf(65536, '\0');
    udf.replace(32768, 6, "\000BEA01", 6);
    udf.replace(34816, 6, "\000NSR02", 6);
    check(udf, true, false);
    for (bool little : {true, false}) {
        std::string encoded(65536, '\0');
        encoded.replace(32768, 6, "\001CD001", 6);
        const auto encode = [little](std::string_view s) {
            std::string result;
            for (char c : s) { result += little ? c : '\0'; result += little ? '\0' : c; }
            return result;
        };
        encoded.replace(40000, 16, encode("BOOT.WIM"));
        encoded.replace(41000, 14, encode("bootmgr"));
        check(encoded, true, true);
    }
}

void progressAndBuffer() {
    std::vector<ProgressInfo> progress;
    for (int i = 0; i < 4096; ++i) {
        progress.emplace_back(std::to_string(i), "fake-device", "4 MB", "source.iso");
        auto& p = progress.back();
        p.completed = i % 2 == 0; p.failed = i % 3 == 0;
        p.bytesWritten = i * 4096u; p.progress = i % 101; p.speed = i * 0.5;
    }
    for (int i = 0; i < 4096; ++i) {
        const auto& p = progress[i];
        CHECK(p.filename == std::to_string(i) && p.device == "fake-device" && p.sourcePath == "source.iso" && p.totalSize == "4 MB");
        CHECK(p.completed.load() == (i % 2 == 0) && p.failed.load() == (i % 3 == 0));
        CHECK(p.bytesWritten.load() == i * 4096u && p.progress.load() == i % 101 && p.speed.load() == i * 0.5);
    }
    ProgressInfo target("", "", "", "");
    target = std::move(progress.back());
    CHECK(target.filename == "4095" && target.bytesWritten.load() == 4095u * 4096u);
    auto& alias = target; target = std::move(alias);
    CHECK(target.filename == "4095" && target.progress.load() == 4095 % 101);
    for (std::size_t alignment : {512, 4096}) {
        AlignedBuffer buffer(8193, alignment);
        CHECK(buffer.ok() && buffer.size == 8193);
        CHECK(reinterpret_cast<std::uintptr_t>(buffer.data) % alignment == 0);
        std::memset(buffer.data, 0x5a, buffer.size);
        CHECK(buffer.data[8192] == 0x5a);
    }
}

} // namespace isocmd_tests
