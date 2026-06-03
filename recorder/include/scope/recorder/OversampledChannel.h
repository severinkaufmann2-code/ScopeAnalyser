#pragma once

#include "scope/core/IAdsClient.h"
#include "scope/core/Signal.h"

#include <concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <vector>

namespace scope::recorder {

// Oversampled-array channel. The PLC project already exposes an
// ARRAY[0..N-1] OF T that an oversampling terminal or fast task fills
// every parent-task tick. We read the whole array per tick (one ADS
// notification with the full array length), then split the array into
// N synthetic samples spaced by `parentTaskCycleUs / N` microseconds.
//
// Stub for Phase 2 — only the interface is committed in Phase 1.
class OversampledChannel {
public:
    struct Config {
        scope::core::Signal::Meta meta;
        std::uint32_t indexGroup;
        std::uint32_t indexOffset;
        std::uint32_t parentTaskCycleUs;
        std::uint32_t oversamplingFactor;  // N from ARRAY[0..N-1]
    };

    explicit OversampledChannel(Config cfg) : cfg_(std::move(cfg)) {}

    bool arm(scope::core::IAdsClient& client, QString* errorOut = nullptr);
    void disarm(scope::core::IAdsClient& client);
    std::size_t drainTo(scope::core::Signal& signal, std::size_t maxBatch = 65536);

private:
    Config cfg_;
    scope::core::AdsNotificationHandle handle_{0};
};

}  // namespace scope::recorder
