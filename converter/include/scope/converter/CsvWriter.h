#pragma once

#include "scope/core/Signal.h"

#include <QString>

#include <filesystem>
#include <memory>
#include <vector>

namespace scope::converter {

struct CsvExportOptions {
    enum class TimeMode {
        Shared,     // single time column on the left; signals linear-interpolated
                    // to the union of all timestamps. Spreadsheet-friendly default.
        PerSignal,  // each signal gets its own (t, value) pair; exact timestamps.
    };

    bool     includeHeader{true};
    QString  columnDelimiter{","};
    QString  rowDelimiter{"\n"};
    QString  decimalSeparator{"."};
    TimeMode timeMode{TimeMode::Shared};
    // Format precision for double values. -1 → "g" with 15 digits (round-trip).
    int      decimalDigits{-1};

    // Emit a "# scope-csv: {json}" line as the very first line of the file.
    // The reader uses this when present to round-trip column roles (X-time,
    // X-frequency, signal), units, and signal names without relying on
    // header heuristics. Most CSV consumers (NumPy / pandas / R) skip `#`
    // comment lines; Excel does not — it'll show the metadata as row 1.
    bool     includeMetadata{true};
};

// Write the given signals to `path` according to `opts`. Returns false and
// sets *errorOut on failure.
bool writeCsv(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<scope::core::Signal>>& channels,
              const CsvExportOptions& opts,
              QString* errorOut = nullptr);

}  // namespace scope::converter
