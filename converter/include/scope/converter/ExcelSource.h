#pragma once

#include "scope/converter/IConverterSource.h"

#include <filesystem>

namespace scope::converter {

class ExcelSource : public IConverterSource {
public:
    explicit ExcelSource(const std::filesystem::path& xlsxPath);
    ~ExcelSource() override;

    QString id() const override { return "excel"; }
    QString displayName() const override { return "Microsoft Excel (.xlsx)"; }

    QStringList listRegions() override;
    std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) override;
    std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile,
        QString* errorOut,
        QStringList* warningsOut = nullptr) override;

private:
    std::filesystem::path path_;
};

}  // namespace scope::converter
