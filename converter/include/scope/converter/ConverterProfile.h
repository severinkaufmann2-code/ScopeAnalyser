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
//                          in the profile with role==XTime or XFrequency)
//   neither set         → fall back to profile-level useSampleRate, or the
//                          first X column in the profile (back-compat).
//
// XTime treats the column's values as seconds (or ms/us/ns per `unit`).
// XFrequency treats them as Hz (or kHz/MHz per `unit`) and tags the
// resulting Y signal with Signal::Domain::Frequency so the Analyser's
// Frequency view picks it up automatically.
struct ColumnMapping {
    enum class Role { Ignore, XTime, XFrequency, Signal };
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
    //   3. collapse runs of equal timestamps per collapseDuplicates
    bool    resetTimeToZero{false};
    double  timeOffsetSec{0.0};

    // What to do when several CSV rows produce the same X-axis
    // timestamp for this Y signal. None passes them through unchanged
    // (current default — round-trips the file). First / Last / Mean
    // collapse each run of equal timestamps into one sample.
    enum class CollapseMode { None, First, Last, Mean };
    CollapseMode collapseDuplicates{CollapseMode::None};

    // What to do when several CSV rows produce distinct timestamps but
    // share the same Y value (staircase pattern — common when a CSV
    // is logged faster than the underlying value updates, e.g. an
    // integer encoder read at 4 kHz). None keeps every sample.
    // KeepFirst keeps only the first sample of each run of equal
    // values; KeepLast keeps the last. Applied after the duplicate-
    // timestamp collapse.
    enum class ValuePlateauMode { None, KeepFirst, KeepLast };
    ValuePlateauMode collapseValuePlateaus{ValuePlateauMode::None};
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
