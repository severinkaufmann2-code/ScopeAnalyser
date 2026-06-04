#pragma once

#include <QString>

#include <filesystem>
#include <vector>

namespace scope::converter {

// A saved column-mapping recipe (`*.scaconv`).
struct ColumnMapping {
    enum class Role { Ignore, XTime, Signal };
    QString columnId;     // "A", "B", ... or numeric for CSV
    Role    role{Role::Ignore};
    QString signalName;   // only used when role == Signal
    QString unit;
    // Optional row range (0-based, inclusive). -1 means "auto" — the importer
    // uses headerRow+1 .. last-row. Set by the selection-driven UI when the
    // user marks a specific cell range as X / Y data.
    int     rowStart{-1};
    int     rowEnd{-1};
};

struct ConverterProfile {
    int version{1};
    QString sourceType;       // "excel", "csv"
    QString sheet;            // Excel-specific
    QString range;            // e.g. "A2:F"
    int     headerRow{1};
    QString decimalSeparator{"."};
    QString columnDelimiter{","};   // CSV: column separator, single char
    QString rowDelimiter{"\n"};     // CSV: row separator, default newline

    // X-axis-from-sample-rate mode: when true, the importer ignores any
    // column with role==XTime and generates timestamps from sampleRateHz.
    bool    useSampleRate{false};
    double  sampleRateHz{0.0};      // always stored in Hz internally
    QString sampleRateDisplayUnit{"Hz"};  // for round-tripping the UI choice

    std::vector<ColumnMapping> columns;

    static ConverterProfile loadFromFile(const std::filesystem::path& path,
                                         QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;
};

}  // namespace scope::converter
