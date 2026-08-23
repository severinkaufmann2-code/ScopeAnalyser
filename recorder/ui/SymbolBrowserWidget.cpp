#include "SymbolBrowserWidget.h"

#include "scope/style/StyleKit.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QStandardItem>
#include <QApplication>
#include <QHeaderView>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <functional>
#include <optional>

namespace scope::recorder::ui {

SymbolBrowserWidget::SymbolBrowserWidget(QWidget* parent) : QWidget(parent) {
    filter_     = new QLineEdit(this);
    filter_->setPlaceholderText("Filter symbols…");
    filter_->setClearButtonEnabled(true);
    tree_       = new QTreeView(this);
    refreshBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Refresh), "Refresh", this);
    refreshBtn_->setToolTip("Re-read the symbol list from the connected source.");
    addBtn_     = new QPushButton(
        scope::style::icon(scope::style::Glyph::Plus), "Add selected", this);
    addBtn_->setToolTip("Add the selected symbols to the recording channels.");

    model_ = new QStandardItemModel(0, 4, this);
    model_->setHorizontalHeaderLabels({"Name", "Type", "Group:Offset", "Bytes"});

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);
    // Keep a branch visible when something inside it matches, so filtering
    // for "ActPos" still finds MAIN.testAxis.NcToPlc.ActPos.
    proxy_->setRecursiveFilteringEnabled(true);
    tree_->setModel(proxy_);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setAlternatingRowColors(true);
    // Structures and arrays are branches, not 400 sibling rows — an AXIS_REF
    // alone expands to ~400 leaves, which flat is unusable.
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setExpandsOnDoubleClick(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    scope::style::installEmptyHint(tree_,
        "Connect to a source to browse its symbols.\n\n"
        "Structures and arrays expand — select a branch to take everything\n"
        "inside it. Filter, select, then “Add selected”.");

    byName_ = new QLineEdit(this);
    byName_->setPlaceholderText("Structure member by name, e.g. MAIN.stAxis.fActPos");
    byName_->setClearButtonEnabled(true);
    byName_->setToolTip(
        "Structure members don't appear in the list above: the PLC's symbol\n"
        "upload has one entry per declared variable, so a structure is a\n"
        "single opaque entry and its members are not enumerated.\n\n"
        "Type the full path and the PLC is asked about it directly — it\n"
        "reports the member's own address, size and type. Nested members and\n"
        "elements of an array of structures work too:\n"
        "  MAIN.stAxis.fActPos\n"
        "  MAIN.aAxes[2].fVelo");
    byNameBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Plus), "Add by name", this);
    byNameBtn_->setToolTip("Ask the PLC about this symbol and add it.");

    auto emitByName = [this] {
        const QString n = byName_->text().trimmed();
        if (!n.isEmpty()) emit addByNameRequested(n);
    };
    connect(byNameBtn_, &QPushButton::clicked, this, emitByName);
    connect(byName_, &QLineEdit::returnPressed, this, emitByName);

    connect(filter_, &QLineEdit::textChanged, proxy_,
            [this](const QString& s){ proxy_->setFilterFixedString(s); });
    connect(refreshBtn_, &QPushButton::clicked, this, &SymbolBrowserWidget::refreshRequested);
    connect(addBtn_,     &QPushButton::clicked, this, &SymbolBrowserWidget::addSelectedRequested);

    auto* top = new QHBoxLayout();
    top->setSpacing(6);
    top->addWidget(filter_, /*stretch=*/1);
    top->addWidget(refreshBtn_);
    top->addWidget(addBtn_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 4, 4);
    layout->setSpacing(6);
    layout->addWidget(scope::style::sectionLabel("PLC symbols", this));
    layout->addLayout(top);
    layout->addWidget(tree_);

    auto* byNameRow = new QHBoxLayout();
    byNameRow->addWidget(byName_, /*stretch=*/1);
    byNameRow->addWidget(byNameBtn_);
    layout->addLayout(byNameRow);
}

namespace {

// Split a fully qualified symbol into the path the tree nests it under:
// "MAIN.testAxis.PlcToNc.Override" -> MAIN / testAxis / PlcToNc / Override,
// and "MAIN.aData[3]" -> MAIN / aData / [3], so an array's elements hang off
// the array rather than sitting beside it.
QStringList symbolPath(const QString& name) {
    QStringList out;
    QString cur;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c == '.') {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
        } else if (c == '[') {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
            const int close = name.indexOf(']', i);
            if (close < 0) { cur += name.mid(i); break; }
            out << name.mid(i, close - i + 1);      // keep the brackets: "[3]"
            i = close;
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) out << cur;
    if (out.isEmpty()) out << name;
    return out;
}

// The index tuple of an "[3]" / "[1,2]" node, or nullopt if it isn't one.
std::optional<QList<long long>> arrayIndexOf(const QString& label) {
    if (label.size() < 3 || !label.startsWith('[') || !label.endsWith(']'))
        return std::nullopt;
    QList<long long> out;
    for (const QString& part : label.mid(1, label.size() - 2).split(',')) {
        bool ok = false;
        const long long v = part.trimmed().toLongLong(&ok);
        if (!ok) return std::nullopt;
        out << v;
    }
    return out.isEmpty() ? std::nullopt : std::optional<QList<long long>>(out);
}

// Put array elements in index order: [0] [1] … [9] [10], not the [0] [1] [10]
// [2] a text sort gives. The PLC lists its symbols alphabetically, so element
// symbols it publishes itself arrive in exactly that wrong order.
//
// Only applied where EVERY child is an index — struct members keep the
// declaration order the type table gave them, which mirrors the PLC's memory
// layout and is more useful than alphabetical.
void sortArrayChildren(QStandardItem* node) {
    for (int r = 0; r < node->rowCount(); ++r)
        if (auto* c = node->child(r, 0)) sortArrayChildren(c);

    if (node->rowCount() < 2) return;
    QList<QList<long long>> keys;
    for (int r = 0; r < node->rowCount(); ++r) {
        auto* c = node->child(r, 0);
        const auto k = c ? arrayIndexOf(c->text()) : std::nullopt;
        if (!k) return;                       // not a pure array node
        keys << *k;
    }

    QList<int> order;
    for (int r = 0; r < node->rowCount(); ++r) order << r;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return std::lexicographical_compare(keys[a].begin(), keys[a].end(),
                                            keys[b].begin(), keys[b].end());
    });

    QList<QList<QStandardItem*>> rows;
    rows.reserve(node->rowCount());
    while (node->rowCount() > 0) rows << node->takeRow(0);
    for (int i : order) node->appendRow(rows[i]);
}

}  // namespace

void SymbolBrowserWidget::setSymbols(std::vector<scope::core::AdsSymbol> symbols) {
    symbols_ = std::move(symbols);
    model_->removeRows(0, model_->rowCount());

    // Branch nodes by full path prefix, so every symbol finds or creates its
    // parents once. A prefix that is itself a symbol (a structure) reuses the
    // same node rather than getting a duplicate.
    QHash<QString, QStandardItem*> branches;

    auto makeRow = [this](const QString& label, int symIndex) {
        auto* nameItem = new QStandardItem(label);
        nameItem->setEditable(false);
        nameItem->setData(symIndex, Qt::UserRole);
        QList<QStandardItem*> row{nameItem};
        for (int i = 0; i < 3; ++i) {
            auto* it = new QStandardItem();
            it->setEditable(false);
            row << it;
        }
        return row;
    };

    // Fill in the type / address / size columns and the not-recordable styling.
    auto describe = [](const QList<QStandardItem*>& row,
                       const scope::core::AdsSymbol& s) {
        row[1]->setText(s.typeName);
        row[2]->setText(QString::number(s.indexGroup, 16) + ":" +
                        QString::number(s.indexOffset, 16));
        row[3]->setText(QString::number(s.size));
        if (!s.unsupported) return;
        const QString why =
            QString("%1 has no single numeric value, so it can't be recorded "
                    "on its own.\n\nExpand it and take the members or "
                    "elements inside — or select this row to add all of them "
                    "at once.")
                .arg(s.typeName.isEmpty() ? QString("This symbol")
                                          : QString("'%1'").arg(s.typeName));
        for (auto* it : row) it->setToolTip(why);
        // Dimmed, not disabled: a branch has to stay selectable so it can
        // stand in for everything beneath it.
        const QColor dim = qApp->palette().color(QPalette::Disabled,
                                                 QPalette::WindowText);
        for (auto* it : row) it->setForeground(dim);
    };

    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        const auto& s = symbols_[i];
        const QStringList path = symbolPath(s.name);

        QStandardItem* parent = model_->invisibleRootItem();
        QString prefix;
        for (int d = 0; d < path.size(); ++d) {
            const bool leaf = (d == path.size() - 1);
            prefix += (prefix.isEmpty() || path[d].startsWith('['))
                          ? path[d] : "." + path[d];
            if (leaf) {
                // A symbol that already exists as a branch (a structure whose
                // members were listed first) just gets its own columns filled.
                if (auto* existing = branches.value(prefix)) {
                    const int r = existing->row();
                    QList<QStandardItem*> row;
                    for (int c = 0; c < 4; ++c)
                        row << (existing->parent() ? existing->parent()->child(r, c)
                                                   : model_->item(r, c));
                    existing->setData(static_cast<int>(i), Qt::UserRole);
                    describe(row, s);
                } else {
                    auto row = makeRow(path[d], static_cast<int>(i));
                    describe(row, s);
                    parent->appendRow(row);
                    branches.insert(prefix, row[0]);
                }
            } else {
                auto* next = branches.value(prefix);
                if (!next) {
                    auto row = makeRow(path[d], -1);   // grouping node
                    parent->appendRow(row);
                    next = row[0];
                    branches.insert(prefix, next);
                }
                parent = next;
            }
        }
    }
    sortArrayChildren(model_->invisibleRootItem());
    tree_->collapseAll();
}

std::vector<scope::core::AdsSymbol> SymbolBrowserWidget::selectedSymbols() const {
    std::vector<scope::core::AdsSymbol> out;
    QSet<int> taken;

    // Selecting a branch means "everything recordable inside it" — otherwise a
    // structure would have to be opened and every member ticked by hand.
    std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& src) {
        const int idx = src.data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(symbols_.size())) {
            if (!symbols_[idx].unsupported && !taken.contains(idx)) {
                taken.insert(idx);
                out.push_back(symbols_[idx]);
            }
        }
        for (int r = 0; r < model_->rowCount(src); ++r)
            collect(model_->index(r, 0, src));
    };

    for (const auto& idx : tree_->selectionModel()->selectedRows(0))
        collect(proxy_->mapToSource(idx));
    return out;
}

QStringList SymbolBrowserWidget::selectedGroupNames() const {
    QStringList out;
    for (const auto& idx : tree_->selectionModel()->selectedRows(0)) {
        const auto src = proxy_->mapToSource(idx);
        if (model_->rowCount(src) == 0) continue;      // a leaf stands for itself
        // Full dotted path, so the prompt names what the user actually clicked.
        QStringList parts;
        for (QModelIndex i = src; i.isValid(); i = i.parent()) {
            const QString t = i.data().toString();
            parts.prepend(t.startsWith('[') ? t : (parts.isEmpty() ? t : t + "."));
        }
        QString full;
        for (const auto& p : parts) full += p;
        out << (full.isEmpty() ? src.data().toString() : full);
    }
    return out;
}

}  // namespace scope::recorder::ui
