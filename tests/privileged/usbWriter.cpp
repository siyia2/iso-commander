// SPDX-License-Identifier: GPL-3.0-or-later

#include <linux/loop.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "../common/testHarness.h"

import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.core.cancellation;

using namespace isocmd_tests;
namespace isocmd_tests { int mountTest(); }

int main(int argc,char** argv) {
    // Accept only a whole Linux loop device bound to our private regular file.
    if (argc!=2 || geteuid()!=0) return 2;
    if (std::string_view(argv[1])=="--mount-test") return mountTest();
    try {
        const char* env=std::getenv("ISOCMD_TEST_SANDBOX"); CHECK(env);
        const auto sandbox=fs::canonical(env), backing=sandbox/"backing.img";
        CHECK(fs::current_path()==sandbox);
        struct stat ds{}, bs{}, ss{};
        CHECK(lstat(sandbox.c_str(),&ss)==0 && S_ISDIR(ss.st_mode) && ss.st_uid==0 && (ss.st_mode&077)==0);
        CHECK(lstat(backing.c_str(),&bs)==0 && S_ISREG(bs.st_mode) && bs.st_uid==0 && bs.st_nlink==1);
        const std::string device=argv[1];
        CHECK(device.starts_with("/dev/loop") && device.size()>9 &&
              std::all_of(device.begin()+9,device.end(),[](char c){return c>='0'&&c<='9';}));
        CHECK(lstat(device.c_str(),&ds)==0 && S_ISBLK(ds.st_mode) && major(ds.st_rdev)==7);
        // Keep an open descriptor to prevent automatic loop teardown/reuse.
        ReadFd pinned(device); struct loop_info64 info{};
        CHECK(ioctl(pinned.fd,LOOP_GET_STATUS64,&info)==0 && info.lo_inode==bs.st_ino &&
              info.lo_device==static_cast<std::uint64_t>(bs.st_dev) && info.lo_offset==0 && info.lo_sizelimit==0);
        int sector=0, physical=0; CHECK(ioctl(pinned.fd,BLKSSZGET,&sector)==0 && sector>0);
        if (ioctl(pinned.fd,BLKPBSZGET,&physical)==0) sector=std::max(sector,physical);
        std::uint64_t capacity=0; CHECK(ioctl(pinned.fd,BLKGETSIZE64,&capacity)==0 && capacity==static_cast<std::uint64_t>(bs.st_size));
        CHECK(capacity==16*1024*1024);
        // Synthetic valid signature, deliberately not sector-aligned, crosses a 4 MiB chunk boundary.
        std::string image(5*1024*1024+37,'\0');
        for (std::size_t i=0;i<image.size();++i) image[i]=static_cast<char>(i%251);
        image.replace(32768,6,"\001CD001",6);
        const auto src=sandbox/"source.iso"; CHECK(!fs::exists(src)); writeFile(src,image);
        std::vector<ProgressInfo> progress; progress.emplace_back("source.iso",device,"test",src.string());
        CHECK(writeIsoToDevice(src.string(),device,0,progress));
        CHECK(progress[0].completed && !progress[0].failed && progress[0].progress==100);
        const std::size_t padded=(image.size()+sector-1)/sector*sector;
        image.resize(padded,'\0');
        std::string actual(padded,'\0');
        std::size_t done=0;
        while (done<actual.size()) { auto n=pread(pinned.fd,actual.data()+done,actual.size()-done,done); CHECK(n>0); done+=static_cast<std::size_t>(n); }
        CHECK(actual==image);
        // A byte beyond the padded image must remain untouched.
        char tail=1; CHECK(pread(pinned.fd,&tail,1,padded)==1 && tail==0);
        std::cout << "PASS: Linux writer on disposable loop device (data and padding)\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL: privileged: " << e.what() << '\n'; return 1; }
}
