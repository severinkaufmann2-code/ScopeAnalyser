#pragma once

#include "Types.h"

#include <QString>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace scope::core {

// A typed time-series. Storage is two parallel vectors: timestamps (ns) and
// values stored as raw bytes interpreted per `dataType()`. Designed so the
// recorder can append in chunks without per-sample allocation, and so HDF5
// load can mmap into the value buffer.
//
// Thread-safety: append() and snapshotForRead() are protected by an internal
// mutex. Many concurrent readers + one writer is the expected pattern; for
// hot recording paths use the lock-free queue in the recorder and flush to
// Signal::append() from the writer thread.
class Signal {
public:
    // What the "timestamps" axis means. Almost every signal is Time
    // (seconds-since-epoch in ns). FFT output is Frequency — the same
    // 64-bit field encodes Hz × 1e9 instead, and the Analyser plot
    // shows those signals on a separate view.
    enum class Domain : std::uint8_t { Time, Frequency };

    // Sentinel for Meta::sourceStartNs: "no known source origin". A real
    // origin can legitimately be 0 (files imported with relative time), so
    // "unset" needs an impossible value.
    static constexpr TimestampNs kNoSourceStart =
        std::numeric_limits<TimestampNs>::min();

    struct Meta {
        QString name;
        QString unit;
        DataType dataType{DataType::Float64};
        AcquisitionMode mode{AcquisitionMode::AdsNotify};
        double sampleRateHz{0.0};      // 0 = irregular / event-based
        std::uint32_t parentTaskCycleUs{0};
        QString sourceSymbol;          // PLC symbol, file column, or formula
        Domain  domain{Domain::Time};
        // The time origin of the signal this one was derived from (ns).
        // Two uses, both in-memory only (not persisted to files):
        //  - Spectra (RFFTmag/…): the time-domain source's first timestamp,
        //    so revertFFT reconstructs on the original axis.
        //  - Derived time signals that drop leading samples (Slice, Gate):
        //    the source's first timestamp, so the chart anchors the segment
        //    at its true position even when shown alone.
        // kNoSourceStart = unknown (display anchors at the signal's own
        // first sample). Derived-signal functions carry it through.
        TimestampNs sourceStartNs{kNoSourceStart};
    };

    explicit Signal(Meta meta);

    const Meta& meta() const noexcept { return meta_; }
    void setMeta(Meta meta);

    DataType dataType() const noexcept { return meta_.dataType; }
    std::size_t bytesPerSample() const noexcept { return sizeOf(meta_.dataType); }

    // Append a contiguous block of samples. Caller owns `valueBytes`; we copy.
    void append(const TimestampNs* timestamps,
                const std::byte* valueBytes,
                std::size_t count);

    // Snapshot the current size; safe to call from any thread.
    std::size_t sampleCount() const;

    // Read-only access. Returns spans backed by this Signal's internal vectors;
    // the spans are valid until the next append() on the SAME thread that
    // called snapshotForRead(). Other threads may still be appending.
    struct ReadView {
        const TimestampNs* timestamps;
        const std::byte*   values;
        std::size_t        count;
        DataType           dataType;
    };
    ReadView snapshotForRead() const;

    // Convenience for visualisations that don't care about types: returns
    // values cast to double. Slow path; do NOT use on the hot record path.
    std::vector<double> readAsDouble() const;

    // The time-axis origin this signal is anchored/selected against:
    // Meta::sourceStartNs when known (derived signals), else the first
    // sample's timestamp (0 if empty). This is the single rule shared by
    // the chart's axis base and the time windows of Slice/BandZero/BandKeep.
    TimestampNs displayOriginNs() const;

private:
    Meta meta_;
    mutable std::mutex mtx_;
    std::vector<TimestampNs> timestamps_;
    std::vector<std::byte>   values_;
    std::atomic<std::size_t> sampleCount_{0};
};

}  // namespace scope::core
