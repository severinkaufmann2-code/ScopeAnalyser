#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

namespace scope::converter {

class ConverterWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConverterWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);

private:
    scope::core::SignalStore& store_;
};

}  // namespace scope::converter
