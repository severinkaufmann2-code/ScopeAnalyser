#pragma once

#include "scope/converter/IConverterSource.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace scope::converter {

// CSV file source. Reads the entire file into memory once on construction;
// callers then enumerate the single region ("file") and ask for previews or
// apply a profile.
class CsvSource : public IConverterSource {
public:
    explicit CsvSource(const std::filesystem::path& path,
                       QChar delimiter = ',',
                       QChar decimal   = '.');
    ~CsvSource() override;

    QString id() const override { return "csv"; }
    QString displayName() const override { return "Comma-separated values (.csv)"; }

    QStringList listRegions() override { return {"file"}; }
    std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) override;
    std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile, QString* errorOut) override;

    int columnCount() const;
    int rowCount() const;
    QString cell(int row, int col) const;
    QStringList headerRow(int rowIndex) const;

private:
    std::filesystem::path path_;
    QChar  delimiter_;
    QChar  decimal_;
    std::vector<QStringList> rows_;
};

}  // namespace scope::converter
