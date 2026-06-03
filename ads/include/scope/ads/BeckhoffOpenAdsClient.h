#pragma once

#include "scope/core/IAdsClient.h"

#include <memory>

namespace scope::ads {

// Wraps github.com/Beckhoff/ADS (the open-source C++ ADS-over-TCP library).
// Works on Linux and Windows; uses the AMS NetId you assign to your host and
// requires the route to be added on the PLC side (TwinCAT System Manager).
class BeckhoffOpenAdsClient : public scope::core::IAdsClient {
public:
    BeckhoffOpenAdsClient();
    ~BeckhoffOpenAdsClient() override;

    bool connect(const scope::core::AdsRoute& route, QString* errorOut) override;
    void disconnect() override;
    bool isConnected() const override;

    std::vector<scope::core::AdsSymbol> listSymbols(QString* errorOut) override;
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

}  // namespace scope::ads
