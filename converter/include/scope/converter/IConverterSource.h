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

    // Apply a profile and return the resulting signals. If `warningsOut`
    // is non-null, the source appends non-fatal messages there (e.g. an
    // X-axis unit that doesn't match the recognised ladder, which the
    // parser still consumes but with a silent fallback default). The
    // caller decides whether to surface them.
    virtual std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile,
        QString* errorOut = nullptr,
        QStringList* warningsOut = nullptr) = 0;
};

}  // namespace scope::converter
