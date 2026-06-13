#pragma once

#include "scope/converter/TabularSource.h"

#include <filesystem>

namespace scope::converter {

// CSV file source. Reads the entire file into the shared tabular grid once on
// construction; the TabularSource base supplies the preview model and the
// profile-driven apply() engine. Callers then enumerate the single region
// ("file") and ask for previews or apply a profile.
//
// `columnDelimiter` is a single character (e.g. ",", ";", "\t", "|").
// `rowDelimiter` is a string. The default "\n" also accepts "\r\n" (the
// trailing "\r" of each line is stripped). Any other value is treated as an
// exact byte-sequence row separator (useful for files where the whole CSV
// lives on one line separated by some non-newline character).
class CsvSource : public TabularSource {
public:
    explicit CsvSource(const std::filesystem::path& path,
                       QString columnDelimiter = ",",
                       QString rowDelimiter    = "\n",
                       QChar   decimal         = '.');
    ~CsvSource() override;

    QString id() const override { return "csv"; }
    QString displayName() const override { return "Comma-separated values (.csv)"; }
};

}  // namespace scope::converter
