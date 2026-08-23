#pragma once

#include "scope/core/IAdsClient.h"

#include <memory>

namespace scope::ads {

// Synthetic signal source that implements IAdsClient. Generates sine, cosine,
// sawtooth, counter, toggle and noisy sine signals at fixed rates. Lets the
// rest of ScopeAnalyser (Recorder pipeline, HDF5, live plot, Analyser) be
// developed and validated without a TwinCAT runtime.
//
// Catalog:
//   Mock.sine_1hz       LREAL  1 ms   sin(2*pi*t)
//   Mock.cosine_10hz    LREAL  1 ms   cos(20*pi*t)
//   Mock.sawtooth       REAL   5 ms   fmod(t, 1.0)
//   Mock.counter        DINT   10 ms  monotonic ++
//   Mock.toggle         BOOL   10 ms  toggles each tick
//   Mock.noisy_sine     LREAL  1 ms   sin(2*pi*t) + N(0, 0.05)
class MockAdsClient : public scope::core::IAdsClient {
public:
    MockAdsClient();
    ~MockAdsClient() override;

    bool connect(const scope::core::AdsRoute& route, QString* errorOut) override;
    void disconnect() override;
    bool isConnected() const override;

    std::vector<scope::core::AdsSymbol> listSymbols(QString* errorOut) override;
    std::optional<scope::core::AdsSymbol> resolveSymbol(
        const QString& name, QString* errorOut) override;
    std::vector<scope::core::AdsTaskInfo> listTasks(QString* errorOut) override;
    std::uint32_t taskCycleForSymbol(const scope::core::AdsSymbol& symbol) override;

    scope::core::AdsNotificationHandle addNotification(
        const scope::core::AdsNotificationSpec& spec,
        scope::core::AdsNotificationHandler handler,
        QString* errorOut) override;

    void removeNotification(scope::core::AdsNotificationHandle handle) override;

    bool read(std::uint32_t indexGroup,
              std::uint32_t indexOffset,
              std::span<std::byte> out,
              QString* errorOut) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<scope::core::IAdsClient> makeMockAdsClient();

}  // namespace scope::ads
