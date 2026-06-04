#pragma once

#include <QString>

#include <filesystem>
#include <vector>

namespace scope::converter {

// One channel the user has manually added in the Converter. References a
// single column by letter (e.g. "A") and optionally a row range (1-based for
// the UI, stored 0-based here; -1 means "auto", i.e. headerRow+1 .. EOF).
struct ColumnMapping {
    enum class Role { Ignore, XTime, Signal };
    QString columnId;     // "A", "B", ...
    Role    role{Role::Ignore};
    QString signalName;   // only used when role == Signal
    QString unit;
    int     rowStart{-1}; // 0-based inclusive; -1 = auto
    int     rowEnd{-1};   // 0-based inclusive; -1 = auto
};

struct ConverterProfile {
    int version{1};
    QString sourceType;       // "excel", "csv"
    QString sheet;            // Excel-specific
    QString range;            // e.g. "A2:F"
    int     headerRow{1};
    QString decimalSeparator{"."};
    QString columnDelimiter{","};
    QString rowDelimiter{"\n"};

    // X-axis from sample rate. When enabled the importer ignores any column
    // whose role is XTime and synthesises timestamps spaced 1/sampleRateHz
    // apart, starting at 0.
    bool    useSampleRate{false};
    double  sampleRateHz{0.0};
    QString sampleRateDisplayUnit{"ms"};  // round-tripped for the UI

    std::vector<ColumnMapping> columns;

    static ConverterProfile loadFromFile(const std::filesystem::path& path,
                                         QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;
};

}  // namespace scope::converter
