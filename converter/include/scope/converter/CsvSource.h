#pragma once

#include "scope/converter/IConverterSource.h"

#include <filesystem>

namespace scope::converter {

class CsvSource : public IConverterSource {
public:
    explicit CsvSource(const std::filesystem::path& path);
    ~CsvSource() override;

    QString id() const override { return "csv"; }
    QString displayName() const override { return "Comma-separated values (.csv)"; }

    QStringList listRegions() override { return {"file"}; }
    std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) override;
    std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile, QString* errorOut) override;

private:
    std::filesystem::path path_;
};

}  // namespace scope::converter
