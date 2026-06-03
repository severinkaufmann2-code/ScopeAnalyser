#pragma once

#include "scope/core/Signal.h"
#include "scope/converter/ConverterProfile.h"

#include <QAbstractItemModel>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace scope::converter {

// Plug-in interface for file-type-specific converter sources. v1: Excel, CSV.
class IConverterSource {
public:
    virtual ~IConverterSource() = default;

    virtual QString id() const = 0;       // "excel", "csv", ...
    virtual QString displayName() const = 0;

    // Returns a list of selectable regions in the file (sheets, tables...).
    virtual QStringList listRegions() = 0;

    // Returns a model that displays the given region for the user to map.
    virtual std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) = 0;

    // Apply a profile and return the resulting signals.
    virtual std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile, QString* errorOut = nullptr) = 0;
};

}  // namespace scope::converter
