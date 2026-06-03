#include "scope/converter/CsvSource.h"

namespace scope::converter {

CsvSource::CsvSource(const std::filesystem::path& path) : path_(path) {}
CsvSource::~CsvSource() = default;

std::unique_ptr<QAbstractItemModel> CsvSource::previewModel(const QString&) { return {}; }

std::vector<std::shared_ptr<scope::core::Signal>> CsvSource::apply(
    const ConverterProfile&, QString* errorOut) {
    if (errorOut) *errorOut = "CsvSource::apply: Phase 4";
    return {};
}

}  // namespace scope::converter
