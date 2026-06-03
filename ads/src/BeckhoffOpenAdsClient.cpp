#include "scope/ads/BeckhoffOpenAdsClient.h"

#include <spdlog/spdlog.h>

// Beckhoff open-source ADS library headers (fetched via CMake FetchContent).
#include "AdsLib.h"
#include "AdsNotificationOOI.h"
#include "AdsVariable.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace scope::ads {

using namespace scope::core;

namespace {

constexpr std::uint16_t kSystemServicePort = 10000;
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
    std::uint16_t flags;
    std::uint16_t nameLen;
    std::uint16_t typeLen;
    std::uint16_t commentLen;
    // Followed by name[nameLen+1] type[typeLen+1] comment[commentLen+1]
};
#pragma pack(pop)

// Map TwinCAT ADST_* codes to our DataType. Incomplete — extend as needed.
DataType mapAdsDataType(std::uint32_t adst) {
    switch (adst) {
        case 33: return DataType::Bool;     // ADST_BIT
        case 16: return DataType::Int8;     // ADST_INT8
        case 17: return DataType::Uint8;    // ADST_UINT8
        case 2:  return DataType::Int16;    // ADST_INT16
        case 18: return DataType::Uint16;   // ADST_UINT16
        case 3:  return DataType::Int32;    // ADST_INT32
        case 19: return DataType::Uint32;   // ADST_UINT32
        case 20: return DataType::Int64;    // ADST_INT64
        case 21: return DataType::Uint64;   // ADST_UINT64
        case 4:  return DataType::Float32;  // ADST_REAL32
        case 5:  return DataType::Float64;  // ADST_REAL64
        default: return DataType::Float64;
    }
}

}  // namespace

struct BeckhoffOpenAdsClient::Impl {
    std::unique_ptr<AdsDevice> device;
    AdsRoute route;
    std::atomic<bool> connected{false};

    struct Subscription {
        AdsNotificationHandler handler;
        std::uint32_t hNotification;
        AmsAddr ams;
    };
    std::mutex subsMtx;
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
            // Linear scan; subscription count is bounded (~100s) and lookups
            // happen on the ADS callback thread — we don't allocate.
            for (auto& [_, s] : impl->subs) {
                if (s->hNotification == pHeader->hNotification) {
                    sub = s.get();
                    break;
                }
            }
        }
        if (!sub) return;

        AdsSample sample;
        sample.plcTimestampNs  = static_cast<TimestampNs>(pHeader->nTimeStamp) * 100;  // 100ns ticks
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
        // Parse NetId "1.2.3.4.5.6" into AmsNetId.
        AmsNetId netId{route.netId.toStdString()};
        // Use empty string for local; AdsLib will add the route via TwinCAT
        // router if available, otherwise via TCP to the target IP.
        impl_->device = std::make_unique<AdsDevice>(
            /*ipV4=*/ route.netId.section('.', 0, 3).toStdString(),
            netId,
            route.port);
        impl_->route = route;
        impl_->connected = true;
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("ADS connect({}:{}): {}",
                      route.netId.toStdString(), route.port, e.what());
        impl_->device.reset();
        impl_->connected = false;
        return false;
    }
}

void BeckhoffOpenAdsClient::disconnect() {
    if (!impl_->connected.load()) return;
    {
        std::lock_guard lk(impl_->subsMtx);
        for (auto& [_, sub] : impl_->subs) {
            if (impl_->device) {
                try {
                    impl_->device->DeleteNotification(sub->hNotification);
                } catch (...) {}
            }
        }
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
        // Step 1: read symbol upload info to learn total symbol count and bytes.
        struct UploadInfo {
            std::uint32_t nSymbols;
            std::uint32_t nSymSize;
        } info{};
        std::uint32_t bytesRead = 0;
        impl_->device->ReadReqEx2(kAdsigrpSymUploadInfo2, 0, sizeof(info), &info, &bytesRead);

        // Step 2: pull the whole symbol blob in one shot.
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
            s.dataType    = mapAdsDataType(hdr.dataType);
            const std::size_t elemBytes = sizeOf(s.dataType);
            s.arrayLen    = elemBytes > 0 ? (s.size / static_cast<std::uint32_t>(elemBytes)) : 1;

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
    // The TwinCAT System Service (port 10000) exposes task metadata. The
    // exact index-group/offset triple is documented in InfoSys under
    // "TcSysSrv.h". This is a placeholder until validated against a real
    // system; v1 falls back to per-symbol task cycle lookup, see below.
    if (errorOut) *errorOut = "listTasks(): not yet implemented; use taskCycleForSymbol()";
    return {};
}

std::uint32_t BeckhoffOpenAdsClient::taskCycleForSymbol(const AdsSymbol& /*symbol*/) {
    // TODO Phase 1b: query the TwinCAT System Service for the task that owns
    // this symbol and read its CycleTime. Until that's wired up, the recorder
    // uses a user-provided default (e.g., 1 ms) and lets the user override
    // per channel. Returning 0 here tells the caller "unknown".
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
        auto sub = std::make_unique<Impl::Subscription>();
        sub->handler = std::move(handler);
        impl_->device->GetLocalAddress(&sub->ams);

        AdsNotificationAttrib attrib{};
        attrib.cbLength      = spec.length;
        attrib.nTransMode    = (spec.maxAgeUs == 0)
                                   ? ADSTRANS_SERVERCYCLE
                                   : ADSTRANS_SERVERONCHA;
        attrib.nMaxDelay     = 0;
        attrib.nCycleTime    = spec.cycleTimeUs * 10;  // ADS cycle is 100ns ticks

        std::uint32_t hNotif = 0;
        impl_->device->AddNotification(
            spec.indexGroup, spec.indexOffset, attrib,
            &Impl::NotificationCallback,
            reinterpret_cast<std::uintptr_t>(impl_.get()),
            &hNotif);
        sub->hNotification = hNotif;

        const auto handle = impl_->nextHandle.fetch_add(1);
        {
            std::lock_guard lk(impl_->subsMtx);
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
    }
    try {
        if (impl_->device) impl_->device->DeleteNotification(sub->hNotification);
    } catch (const std::exception& e) {
        spdlog::warn("DeleteNotification: {}", e.what());
    }
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

namespace scope::core {
std::unique_ptr<IAdsClient> makeDefaultAdsClient() {
    return std::make_unique<scope::ads::BeckhoffOpenAdsClient>();
}
}  // namespace scope::core
