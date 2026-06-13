#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

#include <memory>

namespace scope::converter {

class ConverterWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConverterWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);
    ~ConverterWidget();

private:
    // Coalesce a burst of edits into a single undo snapshot. No-op while the
    // undo stack is applying a restore. Defined as a member so any handler
    // lambda that already captures `this` can request a commit.
    void scheduleCommit();

    scope::core::SignalStore& store_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::converter
