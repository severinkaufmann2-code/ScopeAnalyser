#include "SymbolBrowserWidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStandardItem>
#include <QHeaderView>

namespace scope::recorder::ui {

SymbolBrowserWidget::SymbolBrowserWidget(QWidget* parent) : QWidget(parent) {
    filter_     = new QLineEdit(this);
    filter_->setPlaceholderText("Filter symbols...");
    tree_       = new QTreeView(this);
    refreshBtn_ = new QPushButton("Refresh", this);
    addBtn_     = new QPushButton("Add selected", this);

    model_ = new QStandardItemModel(0, 4, this);
    model_->setHorizontalHeaderLabels({"Name", "Type", "Group:Offset", "Bytes"});

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);
    tree_->setModel(proxy_);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(filter_, &QLineEdit::textChanged, proxy_,
            [this](const QString& s){ proxy_->setFilterFixedString(s); });
    connect(refreshBtn_, &QPushButton::clicked, this, &SymbolBrowserWidget::refreshRequested);
    connect(addBtn_,     &QPushButton::clicked, this, &SymbolBrowserWidget::addSelectedRequested);

    auto* top = new QHBoxLayout();
    top->addWidget(filter_, /*stretch=*/1);
    top->addWidget(refreshBtn_);
    top->addWidget(addBtn_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(tree_);
}

void SymbolBrowserWidget::setSymbols(std::vector<scope::core::AdsSymbol> symbols) {
    symbols_ = std::move(symbols);
    model_->removeRows(0, model_->rowCount());
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        const auto& s = symbols_[i];
        auto* nameItem = new QStandardItem(s.name);
        nameItem->setData(QVariant::fromValue<int>(static_cast<int>(i)), Qt::UserRole);
        nameItem->setEditable(false);
        auto* typeItem = new QStandardItem(s.typeName);
        typeItem->setEditable(false);
        auto* addrItem = new QStandardItem(QString::number(s.indexGroup, 16) + ":" +
                                           QString::number(s.indexOffset, 16));
        addrItem->setEditable(false);
        auto* sizeItem = new QStandardItem(QString::number(s.size));
        sizeItem->setEditable(false);
        model_->appendRow({nameItem, typeItem, addrItem, sizeItem});
    }
}

std::vector<scope::core::AdsSymbol> SymbolBrowserWidget::selectedSymbols() const {
    std::vector<scope::core::AdsSymbol> out;
    for (const auto& idx : tree_->selectionModel()->selectedRows(0)) {
        const auto srcIdx = proxy_->mapToSource(idx);
        const int symIndex = srcIdx.data(Qt::UserRole).toInt();
        if (symIndex >= 0 && symIndex < static_cast<int>(symbols_.size())) {
            out.push_back(symbols_[symIndex]);
        }
    }
    return out;
}

}  // namespace scope::recorder::ui
