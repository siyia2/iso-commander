// SPDX-License-Identifier: GPL-3.0-or-later

#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include "../common/testHarness.h"

import isocmd.input.inputHandling;
import isocmd.core.cancellation;
import isocmd.ops.cpMvRm;
import isocmd.database.databaseOps;

namespace isocmd_tests {
    void cancellation() {
        struct Restore {
            struct sigaction previous{};
            bool flag = GlobalState::g_operationCancelled.exchange(false);
            Restore() { CHECK(sigaction(SIGINT,nullptr,&previous)==0); }
            ~Restore() { sigaction(SIGINT,&previous,nullptr); GlobalState::g_operationCancelled=flag; }
        } restore;
        TempDir tmp; const auto src=tmp.path/"source.iso", dst=tmp.path/"dest.iso";
        writeFile(src,"source"); writeFile(dst,"sentinel");
        setupSignalHandlerCancellations(); CHECK(raise(SIGINT)==0); CHECK(GlobalState::g_operationCancelled);
        std::atomic<std::size_t> bytes{0}; std::error_code ec;
        CHECK(!bufferedCopyWithProgress(src,dst,&bytes,ec));
        CHECK(ec==std::errc::operation_canceled && bytes==0 && readFile(dst)=="sentinel" && !fs::exists(dst.string()+".part"));
        std::unordered_set<std::string> successes,errors; std::mutex mutex;
        BatchReporter reporter(successes,errors,mutex); std::atomic<bool> ok{true};
        std::atomic<std::size_t> done{0}, failed{0}; OperationContext ctx{reporter,&done,&failed,ok,{},&bytes};
        CHECK(!performMoveOperation(ctx,{src,dst,"test","source.iso","test/","dest.iso"},6));
        performDeleteOperation(ctx,src,"test","source.iso",6);
        CHECK(readFile(src)=="source" && readFile(dst)=="sentinel" && failed==2 && done==0 && !ok);
        std::vector<std::string> files; std::unordered_set<std::string> scanErrors; std::mutex fm,em;
        traverse(tmp.path,files,scanErrors,bytes,fm,em,-1,false);
        CHECK(files.empty() && scanErrors.size()==1);
        GlobalState::g_operationCancelled=false;
        ec.clear(); CHECK(bufferedCopyWithProgress(src,dst,&bytes,ec)); CHECK(readFile(dst)=="source" && bytes==6);
    }
    void activeCancellation() {
        struct Restore {
            struct sigaction previous{};
            bool flag=GlobalState::g_operationCancelled.load();
            Restore(){ CHECK(sigaction(SIGINT,nullptr,&previous)==0); GlobalState::g_operationCancelled=false; }
            ~Restore(){ sigaction(SIGINT,&previous,nullptr); GlobalState::g_operationCancelled=flag; }
        } restore;
        setupSignalHandlerCancellations();
        TempDir tmp; const auto fifo=tmp.path/"stream", dst=tmp.path/"destination.iso";
        CHECK(mkfifo(fifo.c_str(),0600)==0); writeFile(dst,"sentinel");
        std::atomic<std::size_t> bytes{0}; std::error_code ec; bool result=true;
        // FIFO producer supplies exactly one copy chunk and then keeps the pipe
        // open. Copy cannot finish successfully before SIGINT is delivered.
        std::jthread worker;
        struct Pipe { int fd; ~Pipe(){ if(fd>=0) ::close(fd); } } pipe{::open(fifo.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC)};
        CHECK(pipe.fd>=0);
        worker=std::jthread([&]{ result=bufferedCopyWithProgress(fifo,dst,&bytes,ec); });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        std::string chunk(4*1024*1024,'x'); std::size_t sent=0;
        while(sent<chunk.size() && std::chrono::steady_clock::now()<deadline) {
            const auto n=::write(pipe.fd,chunk.data()+sent,chunk.size()-sent);
            if(n>0) sent+=static_cast<std::size_t>(n);
            else if(n<0 && (errno==EAGAIN || errno==EINTR)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else break;
        }
        while(bytes.load()==0 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool active=sent==chunk.size() && bytes.load()==chunk.size();
        const int raised=raise(SIGINT);
        ::close(pipe.fd); pipe.fd=-1; worker.join();
        CHECK(active && raised==0 && GlobalState::g_operationCancelled);
        CHECK(!result && ec==std::errc::operation_canceled && readFile(dst)=="sentinel");
        CHECK(!fs::exists(dst.string()+".part"));
        GlobalState::g_operationCancelled=false; const auto src=tmp.path/"regular.iso";
        writeFile(src,"recovered"); bytes=0; ec.clear();
        CHECK(bufferedCopyWithProgress(src,dst,&bytes,ec) && bytes==9 && readFile(dst)=="recovered");
    }

}
