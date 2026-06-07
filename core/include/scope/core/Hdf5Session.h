#pragma once

#include "Signal.h"

#include <QString>

#include <filesystem>
#include <memory>
#include <vector>

namespace HighFive { class File; }

namespace scope::core {

// A session file on disk holding many channels as HDF5 datasets.
//
// Layout:
//   /metadata          (attributes: name, created, host, plc_route, ...)
//   /channels/<name>/timestamps   (int64 dataset, chunked, compressed)
//   /channels/<name>/values       (typed dataset, chunked, compressed)
//                    + attributes: unit, mode, sampleRateHz, taskCycleUs, ...
//
// Append-only during recording; full datasets when loading. Chunk size is
// tuned for ~1 second of data per channel; compression is gzip level 4 by
// default (zstd if HDF5 was built with the zstd filter).
class Hdf5Session {
public:
    static std::unique_ptr<Hdf5Session> create(const std::filesystem::path& path,
                                               QString* errorOut = nullptr);
    static std::unique_ptr<Hdf5Session> openForRead(const std::filesystem::path& path,
                                                    QString* errorOut = nullptr);

    ~Hdf5Session();

    // Add a channel and prepare its datasets. Call before appending.
    bool addChannel(const Signal::Meta& meta, QString* errorOut = nullptr);

    // Append a chunk of samples for an existing channel.
    bool appendSamples(const QString& channelName,
                       const TimestampNs* timestamps,
                       const std::byte* values,
                       std::size_t count,
                       QString* errorOut = nullptr);

    // Load every channel from a read-opened file into memory as Signal objects.
    std::vector<std::shared_ptr<Signal>> loadAllSignals(QString* errorOut = nullptr);

    // Optional plot-layout blob, stored as a string attribute on a top-level
    // "/metadata" group. The host (Analyser) hands down a PlotLayout JSON so a
    // re-opened .h5 restores axes / view mode / channel assignments. Foreign
    // files simply lack the attribute → readLayout() returns an empty string.
    bool    writeLayout(const QString& json, QString* errorOut = nullptr);
    QString readLayout() const;

    void flush();

private:
    Hdf5Session();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::core
