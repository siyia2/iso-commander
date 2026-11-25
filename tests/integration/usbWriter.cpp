// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.core.cancellation;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.windows.windowsWriterEngine;

namespace isocmd_tests {

void usbWriter() {
    TempDir tmp;
    const auto src=tmp.path/"source.iso", target=tmp.path/"regular-file";
    writeFile(target,"sentinel"); writeFile(src,"invalid");
    std::vector<ProgressInfo> progress;
    progress.emplace_back("source.iso",target.string(),"test",src.string());
    CHECK(!writeIsoToDevice(src.string(),target.string(),0,progress));
    CHECK(progress[0].failed && !progress[0].completed && readFile(target)=="sentinel");
    // A valid non-Windows signature reaches the device ioctl, which must fail on a regular file.
    std::string image(65536,'\0'); image.replace(32768,6,"\001CD001",6); writeFile(src,image);
    progress[0].failed=false;
    CHECK(!writeIsoToDevice(src.string(),target.string(),0,progress));
    CHECK(progress[0].failed && !progress[0].completed && readFile(target)=="sentinel");
    // Exercise the actual Windows writer copy engine on ordinary files only.
    using write2usb_detail::IsoCopyEntry; using write2usb_detail::IsoType;
    using windowsWriter_engine::copy::processEntries;
    const auto fat=tmp.path/"fat", ntfs=tmp.path/"ntfs", empty=tmp.path/"empty";
    writeFile(empty,""); std::vector<char> buf(4096); std::atomic<std::uint64_t> total{0};
    const std::vector<IsoCopyEntry> entries{{{},fat/"empty-dir",0,true},{src,fat/"boot/boot.wim",image.size(),false},
        {src,ntfs/"sources/install.wim",image.size(),false},{empty,ntfs/"empty",0,false}};
    CHECK(processEntries(entries,IsoType::WindowsInstall,ntfs.string(),buf.size(),buf.data(),total));
    CHECK(fs::is_directory(fat/"empty-dir") && readFile(fat/"boot/boot.wim")==image && readFile(ntfs/"sources/install.wim")==image);
    CHECK(fs::file_size(ntfs/"empty")==0 && total==2*image.size());
    // Early EOF after one full chunk: incomplete destination is truncated.
    total=0; CHECK(!processEntries({{src,fat/"short",image.size()+1,false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
    CHECK(fs::file_size(fat/"short")==0 && total==image.size());
    total=0; CHECK(!processEntries({{tmp.path/"missing",fat/"missing",1,false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
    CHECK(total==0 && !fs::exists(fat/"missing"));
    struct Reset { ~Reset(){ GlobalState::g_operationCancelled=false; } } reset;
    GlobalState::g_operationCancelled=true;
    CHECK(!processEntries({{src,fat/"cancelled",image.size(),false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
    CHECK(total==0 && !fs::exists(fat/"cancelled"));
}

}
