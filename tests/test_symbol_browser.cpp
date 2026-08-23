// The Recorder's PLC symbol browser.
//
// Once structures and arrays expand, a single Tc2_MC2.AXIS_REF contributes
// ~400 leaves. Flat, that buries everything else. The browser nests symbols by
// their name path instead, so a structure or array is one collapsible branch.
//
// The other half of making that usable: selecting a branch has to mean
// "everything recordable inside it", or a structure would have to be opened
// and every member ticked by hand.

#include "SymbolBrowserWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTreeView>
#include <QAbstractItemModel>

#include <vector>

using namespace scope::core;

namespace {

void ensureGuiApp() {
    if (!QCoreApplication::instance()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char a0[] = {'t', '\0'};
        static char* argv[] = {a0, nullptr};
        new QApplication(argc, argv);   // NOLINT — see test_save_chart_dialog.cpp
    }
}

AdsSymbol leaf(const char* name, const char* type, std::uint32_t off) {
    AdsSymbol s;
    s.name = name; s.typeName = type;
    s.indexGroup = 0x4040; s.indexOffset = off; s.size = 8;
    s.dataType = DataType::Float64;
    return s;
}

AdsSymbol aggregate(const char* name, const char* type) {
    AdsSymbol s;
    s.name = name; s.typeName = type;
    s.indexGroup = 0x4040; s.adsDataType = 65;
    s.unsupported = true;
    return s;
}

QTreeView* treeOf(QWidget& w) { return w.findChild<QTreeView*>(); }

// Walk the view's model to the node at `path`, expanding as it goes.
QModelIndex find(QTreeView* tv, const QStringList& path) {
    QModelIndex cur;
    for (const QString& part : path) {
        QModelIndex next;
        for (int r = 0; r < tv->model()->rowCount(cur); ++r) {
            const auto c = tv->model()->index(r, 0, cur);
            if (c.data().toString() == part) { next = c; break; }
        }
        if (!next.isValid()) return {};
        tv->expand(next);
        cur = next;
    }
    return cur;
}

}  // namespace

TEST(SymbolBrowserTree, NestsMembersUnderTheirStructureInsteadOfListingThemFlat) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.axis", "AXIS_REF"),
        leaf("MAIN.axis.PlcToNc.Override", "UDINT", 0x100),
        leaf("MAIN.axis.PlcToNc.ExtSetPos", "LREAL", 0x104),
        leaf("MAIN.speed", "LREAL", 0x200),
    });
    auto* tv = treeOf(w);
    ASSERT_NE(tv, nullptr);

    // One top-level row — "MAIN" — not four siblings.
    ASSERT_EQ(tv->model()->rowCount(QModelIndex()), 1);
    EXPECT_EQ(tv->model()->index(0, 0, QModelIndex()).data().toString().toStdString(),
              "MAIN");

    // The members hang off axis/PlcToNc, and the leaf shows its own short name.
    const auto p2n = find(tv, {"MAIN", "axis", "PlcToNc"});
    ASSERT_TRUE(p2n.isValid()) << "PlcToNc should be a branch";
    EXPECT_EQ(tv->model()->rowCount(p2n), 2);
    EXPECT_EQ(tv->model()->index(0, 0, p2n).data().toString().toStdString(), "Override");
}

TEST(SymbolBrowserTree, ArrayElementsHangOffTheArrayNotBesideIt) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.aData", "ARRAY [0..2] OF LREAL"),
        leaf("MAIN.aData[0]", "LREAL", 0x00),
        leaf("MAIN.aData[1]", "LREAL", 0x08),
        leaf("MAIN.aData[2]", "LREAL", 0x10),
    });
    auto* tv = treeOf(w);
    const auto arr = find(tv, {"MAIN", "aData"});
    ASSERT_TRUE(arr.isValid());
    EXPECT_EQ(tv->model()->rowCount(arr), 3);
    EXPECT_EQ(tv->model()->index(0, 0, arr).data().toString().toStdString(), "[0]");
    EXPECT_EQ(tv->model()->index(2, 0, arr).data().toString().toStdString(), "[2]");
}

TEST(SymbolBrowserTree, SelectingABranchTakesEverythingRecordableInsideIt) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.axis", "AXIS_REF"),
        leaf("MAIN.axis.PlcToNc.Override", "UDINT", 0x100),
        leaf("MAIN.axis.PlcToNc.ExtSetPos", "LREAL", 0x104),
        leaf("MAIN.axis.NcToPlc.ActPos", "LREAL", 0x200),
        leaf("MAIN.speed", "LREAL", 0x300),
    });
    auto* tv = treeOf(w);

    // Select the structure itself.
    const auto axis = find(tv, {"MAIN", "axis"});
    ASSERT_TRUE(axis.isValid());
    tv->selectionModel()->select(axis, QItemSelectionModel::ClearAndSelect |
                                       QItemSelectionModel::Rows);

    const auto got = w.selectedSymbols();
    ASSERT_EQ(got.size(), 3u) << "all three members, and nothing outside the axis";
    QStringList names;
    for (const auto& s : got) names << s.name;
    names.sort();
    EXPECT_EQ(names, QStringList({"MAIN.axis.NcToPlc.ActPos",
                                  "MAIN.axis.PlcToNc.ExtSetPos",
                                  "MAIN.axis.PlcToNc.Override"}));
    for (const auto& s : got)
        EXPECT_FALSE(s.unsupported) << "the aggregate itself must not be added";
}

TEST(SymbolBrowserTree, SelectingASingleLeafTakesOnlyThatOne) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.axis", "AXIS_REF"),
        leaf("MAIN.axis.PlcToNc.Override", "UDINT", 0x100),
        leaf("MAIN.axis.PlcToNc.ExtSetPos", "LREAL", 0x104),
    });
    auto* tv = treeOf(w);
    const auto one = find(tv, {"MAIN", "axis", "PlcToNc", "Override"});
    ASSERT_TRUE(one.isValid());
    tv->selectionModel()->select(one, QItemSelectionModel::ClearAndSelect |
                                      QItemSelectionModel::Rows);

    const auto got = w.selectedSymbols();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].name.toStdString(), "MAIN.axis.PlcToNc.Override");
}

// A structure is both a branch and a symbol in its own right; it must not turn
// into two rows.
TEST(SymbolBrowserTree, AStructureListedBeforeItsMembersIsNotDuplicated) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.axis", "AXIS_REF"),
        leaf("MAIN.axis.Override", "UDINT", 0x100),
    });
    auto* tv = treeOf(w);
    const auto main = find(tv, {"MAIN"});
    ASSERT_TRUE(main.isValid());
    EXPECT_EQ(tv->model()->rowCount(main), 1) << "one 'axis' row, not two";

    const auto axis = find(tv, {"MAIN", "axis"});
    ASSERT_TRUE(axis.isValid());
    // …and it kept the aggregate's own type text.
    EXPECT_EQ(tv->model()->index(axis.row(), 1, main).data().toString().toStdString(),
              "AXIS_REF");
}
