#include "scope/converter/ExcelSource.h"

// Phase 4 will use QXlsx to enumerate sheets, populate a preview model,
// apply the mapping profile, and emit shared_ptr<Signal>. Stubbed here
// so the build graph is whole.

namespace scope::converter {

ExcelSource::ExcelSource(const std::filesystem::path& xlsxPath) : path_(xlsxPath) {}
ExcelSource::~ExcelSource() = default;

QStringList ExcelSource::listRegions() { return {}; }
std::unique_ptr<QAbstractItemModel> ExcelSource::previewModel(const QString&) { return {}; }
std::vector<std::shared_ptr<scope::core::Signal>> ExcelSource::apply(
    const ConverterProfile&, QString* errorOut, QStringList*) {
    if (errorOut) *errorOut = "ExcelSource::apply: Phase 4";
    return {};
}

}  // namespace scope::converter
