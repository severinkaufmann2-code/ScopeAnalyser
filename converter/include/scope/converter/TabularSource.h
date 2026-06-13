#pragma once

#include "scope/converter/IConverterSource.h"

#include <memory>
#include <vector>

namespace scope::converter {

// Base for converter sources backed by a 2-D string grid: CSV today, JSON
// (flattened) next. Owns the grid plus everything format-independent — the
// preview model and the profile-driven apply() engine (X-time / X-frequency /
// signal roles, per-channel X source, sample-rate mode, time reset/offset,
// duplicate-timestamp and value-plateau collapse). A subclass only has to
// parse its file into `rows_` (row 0 = the header the mapping panel shows,
// rows 1… = data) and set `sourceLabel_`, then implement id()/displayName().
class TabularSource : public IConverterSource {
public:
    // JSON may later expose several tables; for now both sources present a
    // single region, picked at construction. Subclasses can override.
    QStringList listRegions() override { return {"file"}; }
    std::unique_ptr<QAbstractItemModel> previewModel(const QString& region) override;
    std::vector<std::shared_ptr<scope::core::Signal>> apply(
        const ConverterProfile& profile,
        QString* errorOut,
        QStringList* warningsOut = nullptr) override;

    int         columnCount() const;
    int         rowCount() const;
    QString     cell(int row, int col) const;
    QStringList headerRow(int rowIndex) const;

protected:
    // Filename (no path) stitched into each produced Signal's sourceSymbol.
    QString sourceLabel_;
    std::vector<QStringList> rows_;
};

}  // namespace scope::converter
