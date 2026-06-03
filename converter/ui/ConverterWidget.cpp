#include "scope/converter/ConverterWidget.h"

#include <QLabel>
#include <QVBoxLayout>

namespace scope::converter {

ConverterWidget::ConverterWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    auto* label = new QLabel("Converter — Phase 4 placeholder.\n"
                             "Will host: Excel grid preview, drag-mapping to X/Y signal roles,\n"
                             "save/apply .scaconv profiles.",
                             this);
    label->setAlignment(Qt::AlignCenter);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(label);
}

}  // namespace scope::converter
