#pragma once

#include "scope/core/IAdsClient.h"

#include <QWidget>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QTreeView>
#include <QPushButton>

#include <vector>

namespace scope::recorder::ui {

class SymbolBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit SymbolBrowserWidget(QWidget* parent = nullptr);

    void setSymbols(std::vector<scope::core::AdsSymbol> symbols);
    std::vector<scope::core::AdsSymbol> selectedSymbols() const;

signals:
    void refreshRequested();
    void addSelectedRequested();

private:
    QLineEdit* filter_;
    QTreeView* tree_;
    QPushButton* refreshBtn_;
    QPushButton* addBtn_;
    QStandardItemModel* model_;
    QSortFilterProxyModel* proxy_;
    std::vector<scope::core::AdsSymbol> symbols_;
};

}  // namespace scope::recorder::ui
