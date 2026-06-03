#pragma once

#include "Types.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scope::core {

// AMS NetId, e.g. "5.123.45.67.1.1". Port is the ADS port number (851 for
// the first PLC task by default).
struct AdsRoute {
    QString netId;
    std::uint16_t port{851};
};

// One entry from the symbol upload (`ADSIGRP_SYM_UPLOAD`).
struct AdsSymbol {
    QString name;        // fully qualified: "MAIN.fSpeed"
    QString typeName;    // "REAL", "LREAL", "ARRAY [0..99] OF INT", ...
    DataType dataType;
    std::uint32_t indexGroup;
    std::uint32_t indexOffset;
    std::uint32_t size;       // total bytes (= elemSize * arrayLen)
    std::uint32_t arrayLen;   // 1 for scalars, N for ARRAY[0..N-1]
    QString comment;
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
