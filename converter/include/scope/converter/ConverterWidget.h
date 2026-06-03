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
    scope::core::SignalStore& store_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::converter
