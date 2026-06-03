#pragma once

#include "scope/core/IAdsClient.h"
#include "scope/core/Signal.h"

#include <concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace scope::recorder {

// One channel recorded via ADS notifications. Lifetime:
//   construct -> arm(client) -> [ADS callbacks push into queue]
//                            -> drainTo(signal) called by writer thread
//                            -> disarm()
//
// Thread-safety:
//   - The ADS callback runs on the ADS library's background thread; it
//     enqueues into a lock-free queue and returns immediately.
//   - drainTo() runs on the writer thread; it dequeues into the Signal and
//     optionally an HDF5 session.
//   - arm()/disarm() must run on the controlling thread (e.g., UI).
class NotifyChannel {
public:
    struct Config {
        scope::core::Signal::Meta meta;
        std::uint32_t indexGroup;
        std::uint32_t indexOffset;
        std::uint32_t cycleTimeUs;     // 0 = ServerCycle = parent task cycle
    };

    explicit NotifyChannel(Config cfg);
    ~NotifyChannel();

    NotifyChannel(const NotifyChannel&) = delete;
    NotifyChannel& operator=(const NotifyChannel&) = delete;

    const Config& config() const noexcept { return cfg_; }

    bool arm(scope::core::IAdsClient& client, QString* errorOut = nullptr);
    void disarm(scope::core::IAdsClient& client);

    // Move queued samples into the Signal. Returns number of samples drained.
    std::size_t drainTo(scope::core::Signal& signal, std::size_t maxBatch = 4096);

    std::uint64_t samplesReceived() const noexcept { return receivedCount_.load(); }
    std::uint64_t overruns()        const noexcept { return overrunCount_.load(); }

private:
    struct QueuedSample {
        scope::core::TimestampNs ts;
        std::vector<std::byte>   value;  // copied at receive
    };

    void onAdsSample(const scope::core::AdsSample& s);

    Config cfg_;
    scope::core::AdsNotificationHandle handle_{0};
    moodycamel::ConcurrentQueue<QueuedSample> queue_;
    std::atomic<std::uint64_t> receivedCount_{0};
    std::atomic<std::uint64_t> overrunCount_{0};
    std::size_t expectedSampleBytes_{0};
};

}  // namespace scope::recorder
