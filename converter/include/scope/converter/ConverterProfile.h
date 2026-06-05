#pragma once

#include <QString>

#include <filesystem>
#include <vector>

namespace scope::converter {

// One channel the user has manually added. References a single column by
// letter (e.g. "A") and optionally a row range (-1 means "auto").
//
// For Y signals, the X source is per-channel:
//   useSampleRate=true  → synthesise timestamps from sampleRateHz
//   xSourceColumn != "" → take timestamps from that column (must also exist
//                          in the profile with role==XTime)
//   neither set         → fall back to profile-level useSampleRate, or the
//                          first XTime column in the profile (back-compat).
struct ColumnMapping {
    enum class Role { Ignore, XTime, Signal };
    QString columnId;
    Role    role{Role::Ignore};
    QString signalName;
    QString unit;
    int     rowStart{-1};
    int     rowEnd{-1};

    QString xSourceColumn;            // letter of the X column to use, or empty
    bool    useSampleRate{false};
    double  sampleRateHz{0.0};
    QString sampleRateDisplayUnit{"ms"};

    // Time-axis post-processing applied to this Y signal at import:
    //   1. if resetTimeToZero: subtract this channel's first timestamp
    //      (no-op in sample-rate mode — it already starts at 0)
    //   2. add timeOffsetSec * 1e9 to every timestamp
    bool    resetTimeToZero{false};
    double  timeOffsetSec{0.0};
};

struct ConverterProfile {
    int version{1};
    QString sourceType;
    QString sheet;
    QString range;
    int     headerRow{1};
    QString decimalSeparator{"."};
    QString columnDelimiter{","};
    QString rowDelimiter{"\n"};

    // Profile-level X-from-sample-rate. Kept for backward compatibility with
    // earlier .scaconv files. Applied per Y signal only when that signal has
    // no per-channel X source set.
    bool    useSampleRate{false};
    double  sampleRateHz{0.0};
    QString sampleRateDisplayUnit{"ms"};

    std::vector<ColumnMapping> columns;

    static ConverterProfile loadFromFile(const std::filesystem::path& path,
                                         QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;
};

}  // namespace scope::converter
