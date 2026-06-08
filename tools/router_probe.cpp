#include "scope/ads/RouterAdsClient.h"
#include "scope/core/IAdsClient.h"
#include <QString>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    scope::core::AdsRoute route;
    route.host  = argc > 1 ? argv[1] : "127.0.0.1";
    route.netId = argc > 2 ? argv[2] : "10.0.2.15.1.1";
    route.port  = static_cast<std::uint16_t>(argc > 3 ? atoi(argv[3]) : 851);

    scope::ads::RouterAdsClient c;
    QString err;
    if (!c.connect(route, &err)) {
        printf("CONNECT FAIL: %s\n", err.toStdString().c_str());
        return 1;
    }
    printf("CONNECT OK (host=%s target=%s:%d)\n",
           route.host.toStdString().c_str(), route.netId.toStdString().c_str(), route.port);

    err.clear(); auto syms = c.listSymbols(&err); if(!err.isEmpty()) printf("listSymbols err: %s\n", err.toStdString().c_str());
    printf("symbols: %zu\n", syms.size());
    for (size_t i = 0; i < syms.size() && i < 12; ++i)
        printf("  %-40s %s\n", syms[i].name.toStdString().c_str(),
               syms[i].typeName.toStdString().c_str());

    if (!syms.empty()) {
        const auto& s = syms[0];
        scope::core::AdsNotificationSpec spec;
        spec.indexGroup = s.indexGroup; spec.indexOffset = s.indexOffset;
        spec.length = s.size; spec.cycleTimeUs = 100000; spec.maxAgeUs = 0;
        std::atomic<int> n{0};
        auto h = c.addNotification(spec, [&](const scope::core::AdsSample&) { ++n; }, &err);
        printf("notify '%s' handle=%llu err=%s\n", s.name.toStdString().c_str(),
               (unsigned long long)h, err.toStdString().c_str());
        std::this_thread::sleep_for(std::chrono::seconds(2));
        printf("notifications in 2s: %d\n", n.load());
        c.removeNotification(h);
    }
    c.disconnect();
    return 0;
}
