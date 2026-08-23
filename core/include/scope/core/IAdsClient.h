#pragma once

#include "Types.h"

#include <QString>

#include <optional>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scope::core {

// Connection target for an ADS-over-TCP client.
//
// `host` and `netId` are independent: `host` is where we open the TCP/ADS
// connection (an IPv4 address or hostname, e.g. "127.0.0.1" or a PLC's IP),
// while `netId` is the AMS routing address of the target (e.g.
// "5.123.45.67.1.1"). They are NOT interchangeable — a PLC's AMS NetId rarely
// equals its IP. If `host` is empty we fall back to the NetId's first four
// octets for backwards compatibility.
//
// `localNetId` is our own source AMS NetId. The target must have a route
// configured for it, otherwise it cannot send ADS replies back (ADS error 6,
// "target port not found"). Leave empty to let the library auto-derive
// "<our-ip>.1.1".
struct AdsRoute {
    QString host;             // target IP / hostname for the TCP connection
    QString netId;            // target AMS NetId, e.g. "5.123.45.67.1.1"
    std::uint16_t port{851};  // ADS port (851 = first TC3 PLC task)
    QString localNetId;       // our source AMS NetId (optional; see above)
};

// One entry from the symbol upload (`ADSIGRP_SYM_UPLOAD`).
struct AdsSymbol {
    QString name;        // fully qualified: "MAIN.fSpeed"
    QString typeName;    // "REAL", "LREAL", "ARRAY [0..99] OF INT", ...
    // Every scalar is default-initialised: callers build these field by field
    // and an unset one would otherwise be read as garbage.
    DataType dataType{DataType::Float64};
    std::uint32_t indexGroup{0};
    std::uint32_t indexOffset{0};
    std::uint32_t size{0};       // total bytes
    std::uint32_t arrayLen{1};   // 1 for scalars
    QString comment;

    // Raw ADST_* code straight off the wire. Kept because the mapping to
    // DataType is lossy: 65 (ADST_BIGTYPE) covers every struct, function
    // block and most arrays, and those are not one scalar sample.
    std::uint32_t adsDataType{0};

    // True when the symbol has no scalar DataType we can record — a struct,
    // a function block, an array, a STRING, or a scalar code we don't map.
    // Such a symbol is still worth LISTING (the user wants to see it), but
    // recording it directly would sample raw bytes at its base offset and
    // present them as a number. See recordable().
    bool unsupported{false};
};

// Task metadata read from the TwinCAT System Service (port 10000).
struct AdsTaskInfo {
    std::uint32_t index;
    QString name;
    std::uint32_t cycleTimeUs;     // configured task cycle
    std::uint32_t priority;
    bool oversamplingEnabled;
};

// One notification callback. `data` is borrowed; copy if you need to keep it.
struct AdsSample {
    TimestampNs    plcTimestampNs;  // ADS-reported, may be 0 if unsupported
    TimestampNs    hostTimestampNs; // wall-clock at receipt
    std::span<const std::byte> data;
};

using AdsNotificationHandler = std::function<void(const AdsSample&)>;

// Opaque handle the caller passes back to removeNotification().
using AdsNotificationHandle = std::uint64_t;

// Acquisition spec for a single notification subscription.
struct AdsNotificationSpec {
    std::uint32_t indexGroup;
    std::uint32_t indexOffset;
    std::uint32_t length;          // bytes to read each tick
    std::uint32_t cycleTimeUs;     // 0 = ServerCycle (use parent task)
    std::uint32_t maxAgeUs{0};     // 0 = OnChange off, send every tick
};

// Abstract ADS client. v1 has one impl (Beckhoff/ADS open-source); the
// abstraction keeps the door open for TcAdsDll on Windows later.
class IAdsClient {
public:
    virtual ~IAdsClient() = default;

    virtual bool connect(const AdsRoute& route, QString* errorOut = nullptr) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Upload the PLC's symbol table. Returns empty vector on error.
    virtual std::vector<AdsSymbol> listSymbols(QString* errorOut = nullptr) = 0;

    // Ask the PLC about ONE symbol by its fully qualified name, e.g.
    // "MAIN.stAxis.fActPos" or "MAIN.aAxes[2].fVelo".
    //
    // This is how structure members are reached. The symbol upload lists one
    // entry per DECLARED variable, so a struct appears as a single opaque
    // symbol and its members appear nowhere — their names and byte offsets
    // live only in the ADS data-type table. Resolving by name sidesteps that
    // entirely: the PLC answers with the member's own index group, offset,
    // size and type, which is all a recording needs.
    //
    // Returns nullopt with *errorOut set when the name doesn't exist or the
    // type isn't one we can record.
    virtual std::optional<AdsSymbol> resolveSymbol(const QString& name,
                                                   QString* errorOut = nullptr) {
        if (errorOut)
            *errorOut = "This connection can't look symbols up by name.";
        return std::nullopt;
    }

    // Read all tasks of the PLC (System Service, port 10000).
    virtual std::vector<AdsTaskInfo> listTasks(QString* errorOut = nullptr) = 0;

    // For a given symbol, return the parent task's cycle time in µs. Returns
    // 0 if the relationship cannot be determined.
    virtual std::uint32_t taskCycleForSymbol(const AdsSymbol& symbol) = 0;

    // Subscribe to a notification. The handler runs on the ADS callback
    // thread; do as little as possible (push into a lock-free queue).
    virtual AdsNotificationHandle addNotification(const AdsNotificationSpec& spec,
                                                  AdsNotificationHandler handler,
                                                  QString* errorOut = nullptr) = 0;

    virtual void removeNotification(AdsNotificationHandle handle) = 0;

    // Synchronous bulk read of `length` bytes from `indexGroup:indexOffset`.
    virtual bool read(std::uint32_t indexGroup,
                      std::uint32_t indexOffset,
                      std::span<std::byte> out,
                      QString* errorOut = nullptr) = 0;
};

std::unique_ptr<IAdsClient> makeDefaultAdsClient();

}  // namespace scope::core
