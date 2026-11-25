// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/testHarness.h"
import isocmd.write2usb.util.telemetry;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.ui.display.themes;

namespace isocmd_tests {
    void writeTelemetry() {
        using namespace write_telemetry;
        using namespace std::chrono_literals;
        constexpr std::uint64_t MiB=1024*1024;
        const Clock::time_point start{};
        const auto near=[](std::optional<double> value,double expected) {
            CHECK(value && std::abs(*value-expected)<1e-9);
        };
        CHECK(rawStallTimeoutSeconds==60 && windowsStallTimeoutSeconds==300);
        for (int timeout : {rawStallTimeoutSeconds, windowsStallTimeoutSeconds}) {
            SpeedEstimator speed(start,timeout);
            CHECK(!speed.sample(10*MiB,start+500ms));
            near(speed.sample(20*MiB,start+1s),20);
            near(speed.sample(40*MiB,start+2s),20);
            near(speed.sample(45*MiB,start+3s),17); // slow EMA: 0.2
            near(speed.sample(55*MiB,start+4s),14.9); // 10 MiB/s: 0.3
            CHECK(!speed.sample(55*MiB,start+4500ms));
            CHECK(!speed.sample(55*MiB,start+5s)); // first no-progress sample
            CHECK(!speed.sample(55*MiB,start+5s+std::chrono::seconds(timeout-1)));
            near(speed.sample(55*MiB,start+5s+std::chrono::seconds(timeout)),0);
            near(speed.sample(55*MiB,start+6s+std::chrono::seconds(timeout)),0);
            // Baseline remains at the last real write; the previous EMA is retained.
            const auto resume=start+7s+std::chrono::seconds(timeout);
            const double elapsed=std::chrono::duration<double>(resume-(start+4s)).count();
            near(speed.sample(56*MiB,resume),0.8*14.9+0.2/elapsed);
            CHECK(!speed.sample(56*MiB,resume+1s)); // starts a new stall
            CHECK(!speed.sample(55*MiB,resume+2s)); // regressing counter must not underflow
            near(speed.sample(86*MiB,resume+3s),0.7*(0.8*14.9+0.2/elapsed)+3.0);
            SpeedEstimator burst(start,timeout);
            CHECK(!burst.sample(0,start+1s));
            CHECK(!burst.sample(0,start+2s));
            near(burst.sample(12*MiB,start+3s),4);
        }
        for (int i=0;i<=128;++i) CHECK(transferPercent(i,128)==std::min(i*100/128,99));
        CHECK(transferPercent(1,3)==33 && transferPercent(4,3)==99);
        CHECK(formatSpeed(0)=="0.0 KB/s");
        CHECK(formatSpeed(0.05)=="51.2 KB/s");
        CHECK(formatSpeed(0.1)=="0.1 MB/s");
        CHECK(formatSpeed(12.34)=="12.3 MB/s");
    }

    void usbMappingValidation() {
        const std::vector<std::string> files{"one.iso","two.iso"};
        std::vector<std::string> errors;
        const auto null=fs::canonical("/dev/null").string();
        const auto zero=fs::canonical("/dev/zero").string();
        const auto full=fs::canonical("/dev/full").string();
        const auto map=parseDeviceMappings(" 2 > /dev/zero ; 1 > /dev/null ; 1 > /dev/full ;",files,errors);
        CHECK(errors.empty());
        CHECK(map==std::vector<std::pair<std::size_t,std::string>>({{2,zero},{1,null},{1,full}}));
        auto rejected=[&](std::string_view input, std::string_view expected) {
            parseDeviceMappings(std::string(input),files,errors);
            CHECK(std::any_of(errors.begin(),errors.end(),[&](const auto& e){return e.find(expected)!=std::string::npos;}));
        };
        rejected("1>/dev/null;2>/dev/null","used multiple times");
        rejected("1>/dev/null;2>/dev/../dev/null","used multiple times");
        rejected("0>/dev/null","Invalid index");
        rejected("3>/dev/null","Invalid index");
        rejected("x>/dev/null","Invalid index");
        rejected("1 /dev/null","Invalid pair format");
        rejected("1>relative","Invalid device path");
        rejected("1>/dev/isocmd-test-nonexistent-device","Device not found");
        rejected("1>/dev/null","Missing mapping");
        rejected("","Missing mapping");
        const std::vector<IsoInfo> isos{{"one.iso","one.iso",100,"100 B",1},{"two.iso","two.iso",200,"200 B",2}};
        errors.clear();
        CHECK(validateDevices(map,isos,true,errors).empty());
        CHECK(errors.size()==1 && errors[0].find("Root privileges")!=std::string::npos);
        errors.clear();
        CHECK(validateDevices(map,isos,false,errors).empty() && errors.size()==map.size());
        const auto theme=getWriteTheme();
        CHECK(!WriteValidation::checkCapacity(theme,isos[0],null,100));
        CHECK(WriteValidation::checkCapacity(theme,isos[0],null,99).has_value());
    }
} // namespace isocmd_tests
