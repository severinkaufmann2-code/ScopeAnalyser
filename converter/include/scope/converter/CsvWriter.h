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
};

// Write the given signals to `path` according to `opts`. Returns false and
// sets *errorOut on failure.
bool writeCsv(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<scope::core::Signal>>& channels,
              const CsvExportOptions& opts,
              QString* errorOut = nullptr);

}  // namespace scope::converter
