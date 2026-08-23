#pragma once

#include "scope/core/IAdsClient.h"

#include <QStringList>
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

    // Names of the selected rows that stand for a whole group — a structure,
    // an array, or a grouping node like "MAIN". Non-empty means the user
    // clicked one row and is about to get everything under it, which is worth
    // confirming before it lands in the channel table.
    QStringList selectedGroupNames() const;

signals:
    void refreshRequested();
    void addSelectedRequested();
    // A fully qualified name typed by the user, e.g. "MAIN.stAxis.fActPos".
    // Structure members are reachable only this way: the symbol upload lists
    // one entry per declared variable, so members appear in no list.
    void addByNameRequested(QString name);

private:
    QLineEdit* filter_;
    QTreeView* tree_;
    QPushButton* refreshBtn_;
    QPushButton* addBtn_;
    QLineEdit*   byName_;
    QPushButton* byNameBtn_;
    QStandardItemModel* model_;
    QSortFilterProxyModel* proxy_;
    std::vector<scope::core::AdsSymbol> symbols_;
};

}  // namespace scope::recorder::ui
