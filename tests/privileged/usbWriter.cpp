// SPDX-License-Identifier: GPL-3.0-or-later

#include <linux/loop.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "../common/ioFaults.h"

import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.core.cancellation;

using namespace isocmd_tests;
namespace isocmd_tests {
	int mountTest();
	int windowsWriterTest(const std::string& loop, const std::string& layout);
}

namespace {
	using namespace io_faults;

	void cancelRawWrite() {

		GlobalState::g_operationCancelled = true;

	}

	static void checkRawTransfers(const auto& run, const Identity& input, const Identity& output, std::size_t sector, std::size_t chunk, const std::string& paddedImage) {

		{
			Plan p;
			p[Call::Read].set(input, {error(EINTR), shortTransfer(137), shortTransfer(901)});
			run("EINTR and short reads", p, true, false, paddedImage.size());
		}
		{
			Plan p;
			p[Call::Write].set(output, {error(EINTR), shortTransfer(sector), shortTransfer(sector)});
			run("EINTR and aligned short writes", p, true, false, paddedImage.size());
		}
		{
			Plan p;
			p[Call::Read].set(input, {{}, shortTransfer(137), error(EIO)});
			run("read EIO after a full chunk", p, false, false, chunk);
		}
		{
			Plan p;
			p[Call::Read].set(input, {{}, shortTransfer(137), zero()});
			run("premature EOF after a full chunk", p, false, false, chunk);
		}
		for (const auto& [name, fault] : std::initializer_list<std::pair<const char*, Step>>{
				 {"write EIO after partial write", error(EIO)},
			     {"write ENOSPC after partial write", error(ENOSPC)},
			     {"zero-byte write after partial write", zero()}}) {
			Plan p;
			p[Call::Write].set(output, {shortTransfer(sector), fault});
			run(name, p, false, false, sector);
		}
		{
			Plan p;
			p[Call::Fsync].set(output, {error(EIO)});
			run("final fsync EIO", p, false, false, paddedImage.size());
		}
		{
			Plan p;
			p[Call::Write].set(output, {cancelAfterShort(sector, cancelRawWrite)});
			run("cancellation during an active write", p, false, true, sector);
		}

	}

	static void checkDirectWriteRestarts(const auto& run, const Identity& output, std::size_t sector, const std::string& paddedImage) {

		{
			Plan p;
			p[Call::Write].set(output, {unalignedDirectShort(sector)});
			run("recover unaligned O_DIRECT short write", p, true, false, paddedImage.size(), 1, 1);
		}
		{
			Plan p;
			// First complete chunk and then a sector of the next chunk land normally.
			// An unaligned return must not corrupt an earlier chunk or double-count it.
			p[Call::Write].set(output, {{}, shortTransfer(sector), unalignedDirectShort(sector)});
			run("recover unaligned O_DIRECT short write after progress", p, true, false, paddedImage.size(), 1, 1);
		}
		{
			Plan p;
			// The ninth unaligned result exhausts the eight allowed restarts.
			p[Call::Write].set(output, {
				unalignedDirectShort(sector), unalignedDirectShort(sector), unalignedDirectShort(sector),
				unalignedDirectShort(sector), unalignedDirectShort(sector), unalignedDirectShort(sector),
				unalignedDirectShort(sector), unalignedDirectShort(sector), unalignedDirectShort(sector)});
			run("unaligned O_DIRECT restart limit", p, false, false, sector, 0, 9);
		}

	}

	static void checkRawCleanup(const auto& run, const Identity& output, std::size_t sector, const std::string& paddedImage) {

		{
			Plan p;
			p[Call::Fsync].set(output, {error(EINTR), error(EINTR), {}});
			run("final fsync retries EINTR", p, true, false, paddedImage.size(), 3);
		}
		{
			Plan p;
			p[Call::Fsync].set(output, {error(EINTR), error(EIO)});
			run("final fsync EINTR followed by hard error", p, false, false, paddedImage.size(), 2);
		}
		{
			Plan p;
			p[Call::Write].set(output, {shortTransfer(sector), error(EIO)});
			p[Call::Close].set(output, {error(EIO)});
			run("close EIO after failed write", p, false, false, sector, 0, 0, 1);
		}
		{
			Plan clean;
			run("clean write after injected failures", clean, true, false, paddedImage.size());
		}

	}

	static void checkRawVerification(const auto& run, const Identity& output, std::size_t sector, const std::string& paddedImage) {

		{
			Plan p;
			p[Call::Read].set(output, {error(EINTR), shortTransfer(sector)});
			run("verified direct readback with EINTR and short read", p, true, false, paddedImage.size(), 1, 0, 0, true);
		}
		{
			Plan p;
			p[Call::Read].set(output, {corruptRead()});
			run("verification detects corrupted readback", p, false, false, paddedImage.size(), 1, 0, 0, true);
		}
		{
			Plan p;
			p[Call::Read].set(output, {error(EIO)});
			run("verification readback EIO", p, false, false, paddedImage.size(), 1, 0, 0, true);
		}
		{
			Plan p;
			p[Call::Read].set(output, {cancelAfterShort(sector, cancelRawWrite)});
			run("cancellation during verification", p, false, true, paddedImage.size(), 1, 0, 0, true);
		}

	}

	static void checkRawStatus(const ProgressInfo& row, bool success, bool cancelled, bool verify, std::size_t written, std::size_t paddedSize) {

		CHECK(row.completed.load() == success);
		CHECK(row.writeCompleted.load() == (success || (verify && written == paddedSize)));
		CHECK(row.failed.load() == (!success && !cancelled));
		CHECK(GlobalState::g_operationCancelled.load() == cancelled);
		if (verify && written == paddedSize) {
			CHECK(row.verifying);
			if (success) CHECK(row.bytesVerified == paddedSize);
		}

	}

	static void checkRawAccounting(Plan& plan, const ProgressInfo& row, bool success, bool cancelled, std::size_t written, std::size_t paddedSize, std::size_t actualSize, std::optional<std::size_t> syncCalls) {

		CHECK(plan[Call::Fsync].calls.load() == syncCalls.value_or(
		    written == paddedSize && !cancelled ? 1u : 0u));
		CHECK(row.bytesWritten.load() <= std::min(written, actualSize));
		if (success) {
			CHECK(row.progress.load() == 100);
			CHECK(row.bytesWritten.load() == actualSize);
		}
		else CHECK(row.progress.load() < 100);

	}

	// Called only after the existing privileged fixture has pinned the loop device
	// and verified its exact root-owned disposable backing inode and capacity.
	void rawIoFaults(const fs::path& src, const std::string& device, int pinned,
	                 std::size_t sector, const std::string& paddedImage) {

		ReadFd inputFd(src);
		const Identity input(inputFd.fd), output(pinned);
		struct WritableFd {
			int fd;

			~WritableFd() {

				if (fd >= 0) close(fd);

			}
		} seedFd{open(device.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
		CHECK(seedFd.fd >= 0 && output.matches(seedFd.fd));
		const std::size_t actualSize = fs::file_size(src);
		const std::size_t chunk = (4 * 1024 * 1024 / sector) * sector;
		CHECK(chunk > sector && paddedImage.size() > chunk);
		const std::string sentinel(paddedImage.size() + sector, '\x5a');
		auto resetDevice = [&] {

			std::size_t done = 0;
			while (done < sentinel.size()) {
				const auto n = pwrite(seedFd.fd, sentinel.data() + done, sentinel.size() - done, done);
				if (n < 0 && errno == EINTR) continue;
				CHECK(n > 0); done += static_cast<std::size_t>(n);
			}
			CHECK(fsync(seedFd.fd) == 0);

		};
		auto checkDevice = [&](std::size_t written) {

			CHECK(written <= paddedImage.size());
			std::string actual(sentinel.size(), '\0');
			std::size_t done = 0;
			while (done < actual.size()) {
				const auto n = pread(pinned, actual.data() + done, actual.size() - done, done);
				if (n < 0 && errno == EINTR) continue;
				CHECK(n > 0); done += static_cast<std::size_t>(n);
			}
			std::string expected = sentinel;
			expected.replace(0, written, paddedImage, 0, written);
			CHECK(actual == expected);

		};
		int failures = 0;
		auto run = [&](const char* name, Plan& plan, bool success, bool cancelled, std::size_t written,
		               std::optional<std::size_t> syncCalls = {}, std::size_t unalignedEffects = 0,
		               std::size_t closeEffects = 0, bool verify = false) {

			try {
				struct Reset { ~Reset() {

						GlobalState::g_operationCancelled = false;

					} } reset;

				GlobalState::g_operationCancelled = false;
				resetDevice();
				// Count real final flushes even in cases that do not inject a sync fault.
				if (!plan[Call::Fsync].target) plan[Call::Fsync].set(output, {});
				std::vector<ProgressInfo> progress;
				progress.emplace_back("source.iso", device, "test", src.string());
				progress[0].verifyRequested = verify;
				bool result;
				{
					Scope scope(plan);
					result = writeIsoToDevice(src.string(), device, 0, progress);
				}
				CHECK(plan[Call::Write].effects.load() == unalignedEffects);
				CHECK(plan[Call::Close].effects.load() == closeEffects);
				CHECK(result == success);
				for (const auto& rule : plan.rules) CHECK(rule.consumed());
				checkRawStatus(progress[0], success, cancelled, verify, written, paddedImage.size());
				checkRawAccounting(plan, progress[0], success, cancelled, written, paddedImage.size(), actualSize, syncCalls);
				checkDevice(written);
				std::cout << "PASS: raw I/O: " << name << '\n';
			} catch (const std::exception& e) {
				++failures;
				std::cerr << "FAIL: raw I/O: " << name << ": " << e.what() << '\n';
			}

		};
		checkRawTransfers(run, input, output, sector, chunk, paddedImage);
		checkDirectWriteRestarts(run, output, sector, paddedImage);
		checkRawCleanup(run, output, sector, paddedImage);
		checkRawVerification(run, output, sector, paddedImage);
		CHECK(failures == 0);

	}
} // namespace

static void checkDisposableDevice(const fs::path& sandbox, const fs::path& backing, const std::string& backingName, const std::string& device, bool verifyOnly, struct stat& bs) {

	CHECK(fs::current_path()==sandbox);
	struct stat ds{}, ss{};
	CHECK(lstat(sandbox.c_str(),&ss)==0 && S_ISDIR(ss.st_mode) && ss.st_uid==0 && (ss.st_mode&077)==0);
	CHECK(lstat(backing.c_str(),&bs)==0 && S_ISREG(bs.st_mode) && bs.st_uid==0 && bs.st_nlink==1);
	if (verifyOnly) {
		CHECK(device==(backingName=="backing.img" ? "/dev/testa" : "/dev/testb"));
	} else {
		CHECK(device.starts_with("/dev/loop") && device.size()>9 &&
		      std::all_of(device.begin()+9,device.end(),[](char c){return c>='0'&&c<='9';}));
	}
	CHECK(lstat(device.c_str(),&ds)==0 && S_ISBLK(ds.st_mode) && major(ds.st_rdev)==7);

}

static void checkRawWrite(const fs::path& sandbox, const std::string& device, int pinnedFd, int sector) {

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
	while (done<actual.size()) { auto n=pread(pinnedFd,actual.data()+done,actual.size()-done,done); CHECK(n>0); done+=static_cast<std::size_t>(n); }
	CHECK(actual==image);
	// A byte beyond the padded image must remain untouched.
	char tail=1; CHECK(pread(pinnedFd,&tail,1,padded)==1 && tail==0);
	std::cout << "PASS: Linux writer on disposable loop device (data and padding)\n";
	rawIoFaults(src, device, pinnedFd, static_cast<std::size_t>(sector), image);

}

static int checkRawLoop(int fd, const struct stat& bs) {

	struct loop_info64 info{};
	CHECK(ioctl(fd,LOOP_GET_STATUS64,&info)==0 && info.lo_inode==bs.st_ino &&
	      info.lo_device==static_cast<std::uint64_t>(bs.st_dev) && info.lo_offset==0 && info.lo_sizelimit==0);
	int sector=0, physical=0; CHECK(ioctl(fd,BLKSSZGET,&sector)==0 && sector>0);
	if (ioctl(fd,BLKPBSZGET,&physical)==0) sector=std::max(sector,physical);
	std::uint64_t capacity=0; CHECK(ioctl(fd,BLKGETSIZE64,&capacity)==0 && capacity==static_cast<std::uint64_t>(bs.st_size));
	CHECK(capacity==16*1024*1024);
	return sector;

}

int main(int argc,char** argv) {

	// The namespace runner can verify aliases without performing any writes.
	if (geteuid()!=0) return 2;
	if (argc==2 && std::string_view(argv[1])=="--mount-test") return mountTest();
	if (argc==4 && std::string_view(argv[1])=="--windows-test") return windowsWriterTest(argv[2], argv[3]);
	const bool verifyOnly=argc==4 && std::string_view(argv[1])=="--verify-loop";
	if (argc!=2 && !verifyOnly) return 2;
	try {
		const char* env=std::getenv("ISOCMD_TEST_SANDBOX"); CHECK(env);
		const auto sandbox=fs::canonical(env);
		const std::string backingName=verifyOnly ? argv[3] : "backing.img";
		CHECK(backingName=="backing.img" || backingName=="backing2.img");
		const auto backing=sandbox/backingName;
		struct stat bs{};
		const std::string device=verifyOnly ? argv[2] : argv[1];
		checkDisposableDevice(sandbox, backing, backingName, device, verifyOnly, bs);
		// Keep an open descriptor to prevent automatic loop teardown/reuse.
		ReadFd pinned(device);
		const int sector = checkRawLoop(pinned.fd, bs);
		if (verifyOnly) return 0;
		checkRawWrite(sandbox, device, pinned.fd, sector);
		return 0;
	} catch(const std::exception& e) { std::cerr << "FAIL: privileged: " << e.what() << '\n'; return 1; }

}
