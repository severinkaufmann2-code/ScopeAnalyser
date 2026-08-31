#pragma once

#include "scope/core/IAdsClient.h"

#include <QStringList>
#include <QWidget>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QLabel>
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

    // A non-fatal note about the listing just set — a structure the leaf cap
    // cut short, a data-type table the PLC only partly served, members whose
    // type isn't recordable. Empty hides the line. Without this the user sees
    // a symbol simply not there and has no way to tell an app limit from a
    // PLC that never published it.
    void setNote(const QString& note);
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
    // The way to reach a member the listing couldn't offer — the PLC's
    // data-type table is what expands structures into members, so when it is
    // unavailable, partial, or a structure is too large to list in full,
    // asking the PLC about the one name is the route to it.
    void addByNameRequested(QString name);

private:
    QLineEdit* filter_;
    QTreeView* tree_;
    QPushButton* refreshBtn_;
    QPushButton* addBtn_;
    QLabel*      note_;
    QLineEdit*   byName_;
    QPushButton* byNameBtn_;
    QStandardItemModel* model_;
    QSortFilterProxyModel* proxy_;
    std::vector<scope::core::AdsSymbol> symbols_;
};

}  // namespace scope::recorder::ui
