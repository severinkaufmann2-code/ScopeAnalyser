#pragma once

#include "scope/core/Signal.h"

#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace scope::core { class SignalStore; }

namespace scope::converter {

struct CsvExportOptions {
    enum class TimeMode {
        Shared,     // single time column on the left; signals linear-interpolated
                    // to the union of all timestamps. Spreadsheet-friendly default.
        PerSignal,  // each signal gets its own (t, value) pair; exact timestamps.
    };

    // How the time-domain X column is written:
    //   EpochNs         — the signal's absolute timestamp as integer
    //                     nanoseconds. Lossless and absolute (no rebasing to
    //                     t=0, no double rounding), at the cost of large
    //                     integers that aren't spreadsheet-friendly.
    //   RelativeSeconds — seconds relative to the earliest sample, as a double
    //                     (the legacy, human-readable form).
    // Frequency X is always Hz. On read the unit is recovered from the
    // metadata / header and integer-vs-real is detected, so either mode
    // round-trips.
    enum class TimeAxis { EpochNs, RelativeSeconds };

    bool     includeHeader{true};
    QString  columnDelimiter{","};
    QString  rowDelimiter{"\n"};
    QString  decimalSeparator{"."};
    TimeMode timeMode{TimeMode::Shared};
    TimeAxis timeAxis{TimeAxis::EpochNs};
    // Format precision for double values. -1 → "g" with 15 digits (round-trip).
    int      decimalDigits{-1};

    // Emit a "# scope-csv: {json}" line as the very first line of the file.
    // The reader uses this when present to round-trip column roles (X-time,
    // X-frequency, signal), units, and signal names without relying on
    // header heuristics. Most CSV consumers (NumPy / pandas / R) skip `#`
    // comment lines; Excel does not — it'll show the metadata as row 1.
    bool     includeMetadata{true};

    // Optional PlotLayout JSON, embedded as the "layout" key of the
    // scope-csv metadata blob. Set by the Analyser's Save chart… so a
    // re-opened CSV restores per-domain Y axes / channel→axis assignments
    // / view mode / XY-X channel automatically. Stored as a JSON string
    // (via PlotLayout::toJsonString) to avoid leaking nlohmann/json into
    // this public header. Empty → no "layout" key is written, foreign /
    // non-Analyser CSVs are completely unaffected.
    QString  layoutJson;
};

// Write the given signals to `path` according to `opts`. Returns false and
// sets *errorOut on failure.
bool writeCsv(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<scope::core::Signal>>& channels,
              const CsvExportOptions& opts,
              QString* errorOut = nullptr);

// What a TimeMode::Shared write would silently lose.
//
// Shared keys its rows by *distinct* timestamp, so a channel holding several
// samples at one instant only ever gets its first one into the file — one
// row simply has no room for the rest. That is inherent to a single shared X
// column, not a fault of the writer, but it is invisible in the result, so
// the save dialog scans for it up front and says so. TimeMode::PerSignal
// writes raw samples and is unaffected.
struct RepeatedTimestampScan {
    int           channels{0};        // channels that repeat at least one timestamp
    std::int64_t  droppedSamples{0};  // samples a Shared write would omit
    QString       worstChannel;       // the one losing the most, for the message

    bool any() const { return channels > 0; }
};

RepeatedTimestampScan scanRepeatedTimestamps(
    const std::vector<std::shared_ptr<scope::core::Signal>>& channels);
RepeatedTimestampScan scanRepeatedTimestamps(const scope::core::SignalStore& store);

}  // namespace scope::converter
