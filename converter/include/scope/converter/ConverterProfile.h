#pragma once

#include <QChar>
#include <QString>

#include <filesystem>
#include <optional>
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

// Peek the first non-blank line of a CSV-style file. If it starts with
// the "# scope-csv:" sentinel, parse the JSON and build a ConverterProfile
// pre-filled with each column's role / unit / signal name (and refX-
// resolved xSourceColumn pointers for Y signals). Returns true on a
// successful match; the caller can drop the profile into the mapping
// panel so the user just clicks Apply if they're happy. Returns false
// (and leaves `out` untouched) if no metadata is present.
bool profileFromScopeMetadata(const std::filesystem::path& path,
                              ConverterProfile* out,
                              QString* errorOut = nullptr);

// ---- TwinCAT Scope CSV auto-detection ---------------------------------
// A TwinCAT Measurement / Scope export is a Tab- (or ';'-) delimited file
// with a metadata preamble, then a multi-row per-channel header block —
// each channel occupies *two* columns (a time column and a value column),
// keyed in column 0 by "Name" / "Data-Type" / "SampleTime[ms]" / "Unit" /
// … — followed by data rows of alternating (time_ms, value) pairs. Each
// channel runs at its own rate, so the value/time columns are ragged
// (a channel that ends early leaves its cells empty for later rows). This
// layout can't be expressed by the generic single-header CSV loader, so we
// detect + parse it directly.
struct TwinCatScopeChannel {
    QString name;
    QString unit;
    int     timeCol{-1};
    int     valueCol{-1};
    double  sampleTimeMs{0.0};
};
struct TwinCatScopeLayout {
    QChar delimiter{QChar('\t')};
    QChar decimal{QChar('.')};
    int   nameRow{-1};        // 0-based line index of the channel "Name" row
    int   dataStartRow{-1};   // 0-based line index of the first data row
    std::vector<TwinCatScopeChannel> channels;
};

// Peek the header of `path`; return a layout iff it looks like a TwinCAT
// Scope export. Reads only the first ~100 lines, so it's cheap even on a
// multi-hundred-MB file.
std::optional<TwinCatScopeLayout> detectTwinCatScopeCsv(
    const std::filesystem::path& path);

// Build a ConverterProfile from a TwinCAT Scope export — delimiter /
// decimal / data-start row plus one XTime[ms] column and one pinned
// Y-signal column per channel — so the Converter's mapping panel fills in
// with one click. Returns false (and leaves `out` untouched) if the file
// isn't a TwinCAT Scope export.
bool profileFromTwinCatScope(const std::filesystem::path& path,
                             ConverterProfile* out,
                             QString* errorOut = nullptr);

}  // namespace scope::converter
