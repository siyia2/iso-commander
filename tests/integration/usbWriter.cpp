// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.core.cancellation;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.windows.windowsWriterEngine;
import isocmd.write2usb.util.checksum;

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
		std::vector<std::uint64_t> checksums;
		CHECK(processEntries(entries,IsoType::WindowsInstall,ntfs.string(),buf.size(),buf.data(),total,&checksums));
		usb_verify::Checksum64 expected; expected.update(image.data(),image.size());
		CHECK(checksums == std::vector<std::uint64_t>({expected.value,expected.value,0}));
		CHECK(fs::is_directory(fat/"empty-dir") && readFile(fat/"boot/boot.wim")==image && readFile(ntfs/"sources/install.wim")==image);
		CHECK(fs::file_size(ntfs/"empty")==0 && total==2*image.size());
		ProgressInfo verification("source.iso","fixture","test",src.string());
		usb_verify::ReadbackProgress readback(verification,2*image.size());
		CHECK(usb_verify::verifyFile(fat/"boot/boot.wim",image.size(),checksums[0],buf.data(),buf.size(),readback));
		CHECK(usb_verify::verifyFile(ntfs/"sources/install.wim",image.size(),checksums[1],buf.data(),buf.size(),readback));
		CHECK(usb_verify::verifyFile(ntfs/"empty",0,checksums[2],buf.data(),buf.size(),readback));
		CHECK(verification.bytesVerified == 2*image.size());
		image.back() ^= 1; writeFile(ntfs/"sources/install.wim",image); image.back() ^= 1;
		CHECK(!usb_verify::verifyFile(ntfs/"sources/install.wim",image.size(),checksums[1],buf.data(),buf.size(),readback));
		// Early EOF after one full chunk: incomplete destination is truncated.
		total=0; CHECK(!processEntries({{src,fat/"short",image.size()+1,false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
		CHECK(fs::file_size(fat/"short")==0 && total==image.size());
		total=0; CHECK(!processEntries({{tmp.path/"missing",fat/"missing",1,false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
		CHECK(total==0 && !fs::exists(fat/"missing"));

		struct Reset { ~Reset(){

				GlobalState::g_operationCancelled=false;

			} } reset;

		GlobalState::g_operationCancelled=true;
		CHECK(!processEntries({{src,fat/"cancelled",image.size(),false}},IsoType::FatOnly,"",buf.size(),buf.data(),total));
		CHECK(total==0 && !fs::exists(fat/"cancelled"));

	}

}
