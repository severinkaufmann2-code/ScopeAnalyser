#include <optional>
#include "scope/ads/BeckhoffOpenAdsClient.h"

#include <spdlog/spdlog.h>

// Beckhoff open-source ADS library headers (fetched via CMake FetchContent).
#include "AdsLib.h"
#include "AdsDevice.h"
#include "AdsNotificationOOI.h"
#include "AdsVariable.h"

#include <QHash>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace scope::ads {

using namespace scope::core;

namespace {

constexpr std::uint32_t kAdsigrpSymUploadInfo2 = 0xF00F;
constexpr std::uint32_t kAdsigrpSymUpload      = 0xF00B;

// Wire-format header for one entry of ADSIGRP_SYM_UPLOAD (TwinCAT ADS spec).
#pragma pack(push, 1)
struct SymbolEntryHeader {
    std::uint32_t entryLength;
    std::uint32_t indexGroup;
    std::uint32_t indexOffset;
    std::uint32_t size;
    std::uint32_t dataType;   // ADST_* code
    // 4 bytes, not 2 — matches Beckhoff's own AdsSymbolEntry (AdsDef.h). As
    // uint16_t this struct was 28 bytes against a 30-byte wire record, so
    // every name / type / comment after it was read two bytes early and the
    // whole symbol list came back as garbage.
    std::uint32_t flags;
    std::uint16_t nameLen;
    std::uint16_t typeLen;
    std::uint16_t commentLen;
    // Followed by name[nameLen+1] type[typeLen+1] comment[commentLen+1]
};
#pragma pack(pop)

// Map TwinCAT ADST_* codes to our DataType. Shares the router client's rule:
// no honest mapping means nullopt, never a silent Float64.
std::optional<DataType> mapAdsDataTypeOpt(std::uint32_t adst) {
    switch (adst) {
        case 33: return DataType::Bool;
        case 16: return DataType::Int8;
        case 17: return DataType::Uint8;
        case 2:  return DataType::Int16;
        case 18: return DataType::Uint16;
        case 3:  return DataType::Int32;
        case 19: return DataType::Uint32;
        case 20: return DataType::Int64;
        case 21: return DataType::Uint64;
        case 4:  return DataType::Float32;
        case 5:  return DataType::Float64;
        default: return std::nullopt;   // BIGTYPE (struct/FB/array), STRING, …
    }
}

}  // namespace

struct BeckhoffOpenAdsClient::Impl {
    std::unique_ptr<AdsDevice> device;
    AdsRoute route;
    std::atomic<bool> connected{false};

    struct Subscription {
        AdsNotificationHandler handler;
        AdsHandle              adsHandle;  // RAII; destructor removes the notification
        std::uint32_t          notificationId{0};

        Subscription(AdsNotificationHandler h, AdsHandle ah, std::uint32_t id)
            : handler(std::move(h)), adsHandle(std::move(ah)), notificationId(id) {}
    };

    std::mutex subsMtx;
    // Notification dispatch by the PLC-side notification handle (id we get
    // when the AdsHandle is materialized — its `*adsHandle` value).
    std::unordered_map<std::uint32_t, Subscription*> byNotificationId;
    std::unordered_map<AdsNotificationHandle, std::unique_ptr<Subscription>> subs;
    std::atomic<AdsNotificationHandle> nextHandle{1};

    static void NotificationCallback(const AmsAddr* /*pAddr*/,
                                     const AdsNotificationHeader* pHeader,
                                     std::uint32_t hUser) {
        if (!pHeader) return;
        auto* impl = reinterpret_cast<Impl*>(static_cast<std::uintptr_t>(hUser));
        if (!impl) return;
        Subscription* sub = nullptr;
        {
            std::lock_guard lk(impl->subsMtx);
            auto it = impl->byNotificationId.find(pHeader->hNotification);
            if (it != impl->byNotificationId.end()) sub = it->second;
        }
        if (!sub) return;

        AdsSample sample;
        sample.plcTimestampNs  = static_cast<TimestampNs>(pHeader->nTimeStamp) * 100;
        sample.hostTimestampNs = nowNs();
        sample.data = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(pHeader) + sizeof(AdsNotificationHeader),
            pHeader->cbSampleSize);
        sub->handler(sample);
    }
};

BeckhoffOpenAdsClient::BeckhoffOpenAdsClient() : impl_(std::make_unique<Impl>()) {}
BeckhoffOpenAdsClient::~BeckhoffOpenAdsClient() { disconnect(); }

bool BeckhoffOpenAdsClient::connect(const AdsRoute& route, QString* errorOut) {
    try {
        // Our source AMS NetId. The target needs a route back to it; if the
        // caller pins it here, the user can configure that route deterministically.
        if (!route.localNetId.trimmed().isEmpty()) {
            AdsSetLocalAddress(AmsNetId{route.localNetId.trimmed().toStdString()});
        }

        AmsNetId netId{route.netId.toStdString()};

        // TCP target host. Prefer the explicit host; only fall back to the
        // NetId's first four octets when no host was given (legacy behaviour).
        const QString host = route.host.trimmed();
        const std::string target = host.isEmpty()
            ? route.netId.section('.', 0, 3).toStdString()
            : host.toStdString();

        impl_->device = std::make_unique<AdsDevice>(target, netId, route.port);
        impl_->route = route;
        impl_->connected = true;
        spdlog::info("ADS connected: host={} netId={} port={} localNetId={}",
                     target, route.netId.toStdString(), route.port,
                     route.localNetId.toStdString());
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("ADS connect(host={} netId={} port={}): {}",
                      route.host.toStdString(), route.netId.toStdString(),
                      route.port, e.what());
        impl_->device.reset();
        impl_->connected = false;
        return false;
    }
}

void BeckhoffOpenAdsClient::disconnect() {
    if (!impl_->connected.load()) return;
    {
        std::lock_guard lk(impl_->subsMtx);
        // Destroying each Subscription's AdsHandle removes its notification.
        impl_->byNotificationId.clear();
        impl_->subs.clear();
    }
    impl_->device.reset();
    impl_->connected = false;
}

bool BeckhoffOpenAdsClient::isConnected() const { return impl_->connected.load(); }

std::vector<AdsSymbol> BeckhoffOpenAdsClient::listSymbols(QString* errorOut) {
    if (!impl_->device) {
        if (errorOut) *errorOut = "Not connected";
        return {};
    }
    try {
        struct UploadInfo {
            std::uint32_t nSymbols;
            std::uint32_t nSymSize;
        } info{};
        std::uint32_t bytesRead = 0;
        impl_->device->ReadReqEx2(kAdsigrpSymUploadInfo2, 0, sizeof(info), &info, &bytesRead);

        std::vector<std::byte> blob(info.nSymSize);
        impl_->device->ReadReqEx2(kAdsigrpSymUpload, 0,
                                  static_cast<std::uint32_t>(blob.size()),
                                  blob.data(), &bytesRead);

        std::vector<AdsSymbol> out;
        out.reserve(info.nSymbols);

        const std::byte* p   = blob.data();
        const std::byte* end = blob.data() + blob.size();
        while (p + sizeof(SymbolEntryHeader) <= end) {
            SymbolEntryHeader hdr;
            std::memcpy(&hdr, p, sizeof(hdr));
            if (hdr.entryLength == 0 || p + hdr.entryLength > end) break;

            const char* base = reinterpret_cast<const char*>(p + sizeof(SymbolEntryHeader));
            AdsSymbol s;
            s.name        = QString::fromUtf8(base, hdr.nameLen);
            s.typeName    = QString::fromUtf8(base + hdr.nameLen + 1, hdr.typeLen);
            s.comment     = QString::fromUtf8(base + hdr.nameLen + 1 + hdr.typeLen + 1, hdr.commentLen);
            s.indexGroup  = hdr.indexGroup;
            s.indexOffset = hdr.indexOffset;
            s.size        = hdr.size;
            s.adsDataType = hdr.dataType;
            const auto mapped = mapAdsDataTypeOpt(hdr.dataType);
            s.unsupported = !mapped.has_value();
            s.dataType    = mapped.value_or(DataType::Float64);   // placeholder
            s.arrayLen    = 1;

            out.push_back(std::move(s));
            p += hdr.entryLength;
        }
        return out;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("listSymbols: {}", e.what());
        return {};
    }
}

std::vector<AdsTaskInfo> BeckhoffOpenAdsClient::listTasks(QString* errorOut) {
    if (errorOut) *errorOut = "listTasks(): not yet implemented; use taskCycleForSymbol()";
    return {};
}

std::uint32_t BeckhoffOpenAdsClient::taskCycleForSymbol(const AdsSymbol& /*symbol*/) {
    // TODO Phase 1b: query the TwinCAT System Service for the task that owns
    // this symbol and read its CycleTime. Until then, the recorder uses a
    // user-provided default and lets the user override per channel.
    return 0;
}

AdsNotificationHandle BeckhoffOpenAdsClient::addNotification(
    const AdsNotificationSpec& spec,
    AdsNotificationHandler handler,
    QString* errorOut) {

    if (!impl_->device) {
        if (errorOut) *errorOut = "Not connected";
        return 0;
    }
    try {
        AdsNotificationAttrib attrib{};
        attrib.cbLength      = spec.length;
        attrib.nTransMode    = (spec.maxAgeUs == 0)
                                   ? ADSTRANS_SERVERCYCLE
                                   : ADSTRANS_SERVERONCHA;
        attrib.nMaxDelay     = 0;
        attrib.nCycleTime    = spec.cycleTimeUs * 10;  // ADS cycle is 100ns ticks

        auto adsHandle = impl_->device->GetHandle(
            spec.indexGroup, spec.indexOffset, attrib,
            &Impl::NotificationCallback,
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(impl_.get())));

        const std::uint32_t notifId = *adsHandle;

        auto sub = std::make_unique<Impl::Subscription>(
            std::move(handler), std::move(adsHandle), notifId);

        const auto handle = impl_->nextHandle.fetch_add(1);
        {
            std::lock_guard lk(impl_->subsMtx);
            impl_->byNotificationId[notifId] = sub.get();
            impl_->subs.emplace(handle, std::move(sub));
        }
        return handle;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("addNotification: {}", e.what());
        return 0;
    }
}

void BeckhoffOpenAdsClient::removeNotification(AdsNotificationHandle handle) {
    std::unique_ptr<Impl::Subscription> sub;
    {
        std::lock_guard lk(impl_->subsMtx);
        auto it = impl_->subs.find(handle);
        if (it == impl_->subs.end()) return;
        sub = std::move(it->second);
        impl_->subs.erase(it);
        impl_->byNotificationId.erase(sub->notificationId);
    }
    // sub's destructor releases the AdsHandle, which removes the notification.
}

bool BeckhoffOpenAdsClient::read(std::uint32_t indexGroup,
                                 std::uint32_t indexOffset,
                                 std::span<std::byte> out,
                                 QString* errorOut) {
    if (!impl_->device) {
        if (errorOut) *errorOut = "Not connected";
        return false;
    }
    try {
        std::uint32_t bytesRead = 0;
        impl_->device->ReadReqEx2(
            indexGroup, indexOffset,
            static_cast<std::uint32_t>(out.size()),
            out.data(), &bytesRead);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

}  // namespace scope::ads

// makeDefaultAdsClient() now lives in RouterAdsClient.cpp — the AMS/TCP
// router-client is the default because it works against the TwinCAT 4026
// router (the open-source peer-router path here does not). BeckhoffOpenAdsClient
// is kept available for callers that explicitly want the peer-router lib.
