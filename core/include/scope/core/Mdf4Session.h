#pragma once

#include "Signal.h"

#include <QString>

#include <filesystem>
#include <memory>
#include <vector>

namespace scope::core {

// Save / load signals as ASAM MDF 4.x (.mf4) files via the third-party
// mdflib. Mirrors the Hdf5Session API.
//
// Layout: one DataGroup per channel, each with a single ChannelGroup
// containing a Master (Float64, sync=Time, unit "s" / "Hz") and a value
// channel (Float64). Per-channel description carries scope-specific meta
// (domain, source_symbol, original data type) as a tiny key=value blob.
//
// Scope limitations (deliberate):
//  - All values are written as Float64 regardless of original dataType.
//    The original type is round-tripped via metadata so the in-memory
//    Signal::Meta reports correctly, but values lose integer precision
//    beyond 2^53.
//  - One DG per channel keeps the writer code straightforward at the cost
//    of file size (negligible for typical recording lengths).
//  - File-level StartTime is set to 0 so the master column stores the
//    absolute X value (epoch-ns/1e9 for time, Hz for frequency). This
//    makes round-trip exact but means StartTime is not meaningful as an
//    epoch — external tools should ignore it.
class Mdf4Session {
public:
    static std::unique_ptr<Mdf4Session> create(const std::filesystem::path& path,
                                               QString* errorOut = nullptr);
    static std::unique_ptr<Mdf4Session> openForRead(const std::filesystem::path& path,
                                                    QString* errorOut = nullptr);

    ~Mdf4Session();

    // Add a channel before any samples are written.
    bool addChannel(const Signal::Meta& meta, QString* errorOut = nullptr);

    // Append samples; lazy-initialises the measurement on the first call.
    bool appendSamples(const QString& channelName,
                       const TimestampNs* timestamps,
                       const std::byte* values,
                       std::size_t count,
                       QString* errorOut = nullptr);

    // Flush remaining queued samples and finalise the file.
    bool finalize(QString* errorOut = nullptr);

    // Load every channel from a read-opened file into memory as Signal objects.
    std::vector<std::shared_ptr<Signal>> loadAllSignals(QString* errorOut = nullptr);

private:
    Mdf4Session();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::core
