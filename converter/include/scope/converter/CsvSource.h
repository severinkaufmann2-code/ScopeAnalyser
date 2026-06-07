#pragma once

#include "scope/converter/IConverterSource.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace scope::converter {

// CSV file source. Reads the entire file into memory once on construction;
// callers then enumerate the single region ("file") and ask for previews or
// apply a profile.
//
// `columnDelimiter` is a single character (e.g. ",", ";", "\t", "|").
// `rowDelimiter` is a string. The default "\n" also accepts "\r\n" (the
// trailing "\r" of each line is stripped). Any other value is treated as an
// exact byte-sequence row separator (useful for files where the whole CSV
// lives on one line separated by some non-newline character).
class CsvSource : public IConverterSource {
public:
    explicit CsvSource(const std::filesystem::path& path,
                       QString columnDelimiter = ",",
                       QString rowDelimiter    = "\n",
                       QChar   decimal         = '.');
    ~CsvSource() override;

    QString id() const override { return "csv"; }
    QString displayName() const override { return "Comma-separated values (.csv)"; }

    QStringList listRegions() override { return {"file"}; }
    std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) override;
    std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile,
        QString* errorOut,
        QStringList* warningsOut = nullptr) override;

    int columnCount() const;
    int rowCount() const;
    QString cell(int row, int col) const;
    QStringList headerRow(int rowIndex) const;

private:
    std::filesystem::path path_;
    QString columnDelimiter_;
    QString rowDelimiter_;
    QChar   decimal_;
    std::vector<QStringList> rows_;
};

}  // namespace scope::converter
