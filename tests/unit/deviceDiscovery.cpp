// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/testHarness.h"
import isocmd.write2usb.ui.write2usbUI;

namespace isocmd_tests {
    void deviceDiscovery() {
        TempDir tmp;
        const DeviceDiscoveryPaths paths{tmp.path/"sys/block", tmp.path/"dev", tmp.path/"mounts"};
        fs::create_directories(paths.sysBlock);
        fs::create_directories(paths.dev/"disk/by-id");
        auto device = [&](const std::string& name, const std::string& removable, bool usb) {
            const auto parent = tmp.path/"devices"/name;
            const auto node = parent/"block"/name;
            fs::create_directories(node/"device");
            if (usb) writeFile(parent/"idVendor", "1234\n");
            writeFile(node/"removable", removable);
            writeFile(node/"device/model", "  Test Drive \t\n");
            fs::create_directory_symlink(node, paths.sysBlock/name);
            writeFile(paths.dev/name, ""); // synthetic nodes, never opened for writing by discovery
        };
        device("sda", "1\n", true); device("sdb", "0\n", true);
        device("sdc", "1\n", false); device("sdd", "bad\n", true);
        device("loop0", "1\n", true); device("ramdisk", "1\n", true);
        device("zram0", "1\n", true); device("sda1", "1\n", true);
        device("sde", "1\n", true); fs::remove(paths.dev/"sde");
        const auto a = (paths.dev/"sda").string();
        const auto alias = paths.dev/"disk/by-id/usb-test";
        fs::create_symlink(paths.dev/"sda", alias);
        CHECK(isFlashDrive(a, paths) && isFlashDrive(alias.string(), paths));
        for (const auto* name : {"sdb", "sdc", "sdd", "sde", "missing"})
            CHECK(!isFlashDrive((paths.dev/name).string(), paths));
        CHECK(!isFlashDrive("/outside/sda", paths));
        CHECK(getDriveName(a, paths) == "Test Drive");
        CHECK(getDriveName((paths.dev/"missing").string(), paths) == "Unknown Drive");
        CHECK(getRemovableDevices(paths) == std::vector<std::string>{a});
        writeFile(paths.mounts, a+"1 /mnt/test ext4 rw 0 0\n"+(paths.dev/"sdbx").string()+" /other ext4 rw 0 0\n");
        CHECK(isDeviceMounted(a, paths));
        CHECK(!isDeviceMounted((paths.dev/"sdb").string(), paths));
        writeFile(paths.mounts, a+" /mnt/test ext4 rw 0 0\n");
        CHECK(isDeviceMounted(a, paths));
        fs::remove(paths.mounts); CHECK(!isDeviceMounted(a, paths));
    }
}
