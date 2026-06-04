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
};

struct ConverterProfile {
    int version{1};
    QString sourceType;       // "excel", "csv"
    QString sheet;            // Excel-specific
    QString range;            // e.g. "A2:F"
    int     headerRow{1};
    QString decimalSeparator{"."};
    QString columnDelimiter{","};   // CSV: column separator, single char (e.g. ",", ";", "\t", "|")
    QString rowDelimiter{"\n"};     // CSV: row separator, default newline
    std::vector<ColumnMapping> columns;

    static ConverterProfile loadFromFile(const std::filesystem::path& path,
                                         QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;
};

}  // namespace scope::converter
