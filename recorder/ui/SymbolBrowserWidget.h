#pragma once

#include "scope/core/IAdsClient.h"

#include <QStringList>
#include <QWidget>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTreeView>
#include <QPushButton>

#include <vector>

class QResizeEvent;

namespace scope::recorder::ui {

class SymbolBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit SymbolBrowserWidget(QWidget* parent = nullptr);

    void setSymbols(std::vector<scope::core::AdsSymbol> symbols);

    // A non-fatal note about the listing just set — a structure the leaf cap
    // cut short, a data-type table the PLC only partly served, members whose
    // type isn't recordable. Empty hides the pane. Without this the user sees
    // a symbol simply not there and has no way to tell an app limit from a
    // PLC that never published it.
    //
    // A listing can come back with half a dozen of these at once, so the pane
    // scrolls and the divider above it drags: however much there is to say,
    // the symbol tree keeps the height the user gave it.
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

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    // Split the height between tree and notes on the notes' first appearance:
    // enough to read the first one, never more than a quarter of the panel.
    void sizeNotePane();

    QLineEdit* filter_;
    QTreeView* tree_;
    QPushButton* refreshBtn_;
    QPushButton* addBtn_;
    QSplitter*      split_;      // tree ↕ notes, draggable
    QWidget*        notePane_;   // heading + note_; hidden when there's nothing to say
    QPlainTextEdit* note_;
    bool            noteShown_{false};  // survives a user-collapsed pane
    bool            noteSizePending_{false};  // notes arrived before a layout did
    QLineEdit*   byName_;
    QPushButton* byNameBtn_;
    QStandardItemModel* model_;
    QSortFilterProxyModel* proxy_;
    std::vector<scope::core::AdsSymbol> symbols_;
};

}  // namespace scope::recorder::ui
