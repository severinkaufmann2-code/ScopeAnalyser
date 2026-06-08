// Standalone ADS connectivity probe using the SAME open-source Beckhoff AdsLib
// the Recorder uses. Run ON the TwinCAT machine. Tries a small matrix of
// (targetIP, localNetId) against a target AMS NetId/port and reports each.
#include "AdsLib.h"
#include "AdsDevice.h"
#include "AdsException.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static void probe(const std::string& ip, const std::string& tgt,
                  uint16_t port, const std::string& local) {
    std::cout << "--- ip=" << ip << " target=" << tgt << " port=" << port
              << " local=" << (local.empty() ? "<auto>" : local) << "\n";
    if (!local.empty()) AdsSetLocalAddress(AmsNetId{local});
    try {
        AdsDevice device{ip, AmsNetId{tgt}, port};
        auto s = device.GetState();
        std::cout << "    CONNECT OK adsState=" << (int)s.ads
                  << " deviceState=" << (int)s.device << "\n";
        uint8_t info[24]; uint32_t br = 0;
        device.ReadReqEx2(0xF00F, 0, sizeof(info), info, &br);
        uint32_t cnt = 0; std::memcpy(&cnt, info, 4);
        std::cout << "    SYMBOLS count=" << cnt << "\n";
    } catch (const AdsException& e) {
        std::cout << "    FAIL AdsException errorCode=" << e.errorCode
                  << " : " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cout << "    FAIL " << e.what() << "\n";
    }
}

int main(int argc, char** argv) {
    std::string tgt  = argc > 1 ? argv[1] : "10.0.2.15.1.1";
    uint16_t    port = argc > 2 ? (uint16_t)atoi(argv[2]) : 851;
    std::vector<std::string> ips    = {"127.0.0.1", "10.0.2.15"};
    std::vector<std::string> locals = {"10.0.2.100.1.1", "10.0.2.15.1.1", "127.0.0.1.1.1", ""};
    for (auto& ip : ips)
        for (auto& loc : locals)
            probe(ip, tgt, port, loc);
    return 0;
}
