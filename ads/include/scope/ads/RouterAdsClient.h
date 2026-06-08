#pragma once

#include "scope/core/IAdsClient.h"

#include <memory>

namespace scope::ads {

// ADS client that speaks the AMS/TCP *router-client* protocol directly: it
// connects to a TwinCAT AMS router (TCP 48898), registers a port
// (AMS_TCP_PORT_CONNECT) and then exchanges ADS commands over that one socket.
//
// This is the protocol the managed TwinCAT.Ads library uses, and it is the
// path that actually works against a TwinCAT 4026 router. (The open-source
// Beckhoff AdsLib acts as its own peer router, which a TwinCAT 4026 router
// rejects on a local/loopback connection — verified on the test VM.)
//
// Because the router assigns our source AMS address, no manual route setup is
// needed for a local PLC; for a remote PLC, point `host` at that machine's
// router. Replies (and notifications) arrive over the same socket, so it works
// the same on Linux and Windows.
//
//   AdsRoute.host  → router IP/host (127.0.0.1 for a local TwinCAT)
//   AdsRoute.netId → target PLC AMS NetId (e.g. 10.0.2.15.1.1)
//   AdsRoute.port  → ADS port (851 for the first TC3 PLC task)
//   AdsRoute.localNetId is ignored (the router assigns our address).
class RouterAdsClient : public scope::core::IAdsClient {
public:
    RouterAdsClient();
    ~RouterAdsClient() override;

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
