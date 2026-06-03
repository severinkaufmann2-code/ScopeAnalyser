#include "scope/recorder/NotifyChannel.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace scope::recorder {

using scope::core::AdsNotificationSpec;
using scope::core::AdsSample;
using scope::core::Signal;
using scope::core::sizeOf;

NotifyChannel::NotifyChannel(Config cfg)
    : cfg_(std::move(cfg)),
      expectedSampleBytes_(sizeOf(cfg_.meta.dataType)) {}

NotifyChannel::~NotifyChannel() = default;

bool NotifyChannel::arm(scope::core::IAdsClient& client, QString* errorOut) {
    if (handle_ != 0) return true;  // already armed
    AdsNotificationSpec spec;
    spec.indexGroup  = cfg_.indexGroup;
    spec.indexOffset = cfg_.indexOffset;
    spec.length      = static_cast<std::uint32_t>(expectedSampleBytes_);
    spec.cycleTimeUs = cfg_.cycleTimeUs;
    spec.maxAgeUs    = 0;

    handle_ = client.addNotification(spec,
        [this](const AdsSample& s) { this->onAdsSample(s); },
        errorOut);
    return handle_ != 0;
}

void NotifyChannel::disarm(scope::core::IAdsClient& client) {
    if (handle_ == 0) return;
    client.removeNotification(handle_);
    handle_ = 0;
}

void NotifyChannel::onAdsSample(const AdsSample& s) {
    if (s.data.size() != expectedSampleBytes_) {
        overrunCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    QueuedSample q;
    q.ts = (s.plcTimestampNs != 0) ? s.plcTimestampNs : s.hostTimestampNs;
    q.value.resize(expectedSampleBytes_);
    std::memcpy(q.value.data(), s.data.data(), expectedSampleBytes_);

    if (!queue_.enqueue(std::move(q))) {
        overrunCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    receivedCount_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t NotifyChannel::drainTo(Signal& signal, std::size_t maxBatch) {
    std::vector<scope::core::TimestampNs> tsBuf;
    std::vector<std::byte>                valBuf;
    tsBuf.reserve(maxBatch);
    valBuf.reserve(maxBatch * expectedSampleBytes_);

    QueuedSample item;
    std::size_t drained = 0;
    while (drained < maxBatch && queue_.try_dequeue(item)) {
        tsBuf.push_back(item.ts);
        valBuf.insert(valBuf.end(), item.value.begin(), item.value.end());
        ++drained;
    }
    if (drained > 0) {
        signal.append(tsBuf.data(), valBuf.data(), drained);
    }
    return drained;
}

}  // namespace scope::recorder
