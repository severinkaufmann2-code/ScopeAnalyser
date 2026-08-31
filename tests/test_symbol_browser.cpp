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
#include <QLabel>
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

// The PLC lists its symbols alphabetically, so element symbols it publishes
// itself arrive as [0] [1] [10] [11] [2] … Reading an array in that order is
// useless, so the tree orders index nodes numerically whatever came in.
TEST(SymbolBrowserTree, ArrayElementsReadInIndexOrderNotTextOrder) {
    ensureGuiApp();
    std::vector<AdsSymbol> syms{aggregate("MAIN.aData", "ARRAY [0..11] OF LREAL")};
    // Deliberately alphabetical, the order a PLC would hand them over in.
    for (const char* i : {"0","1","10","11","2","3","4","5","6","7","8","9"})
        syms.push_back(leaf(qPrintable(QString("MAIN.aData[%1]").arg(i)),
                            "LREAL", 0));

    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols(syms);
    auto* tv = treeOf(w);
    const auto arr = find(tv, {"MAIN", "aData"});
    ASSERT_TRUE(arr.isValid());
    ASSERT_EQ(tv->model()->rowCount(arr), 12);

    QStringList got;
    for (int r = 0; r < tv->model()->rowCount(arr); ++r)
        got << tv->model()->index(r, 0, arr).data().toString();
    EXPECT_EQ(got, QStringList({"[0]","[1]","[2]","[3]","[4]","[5]",
                               "[6]","[7]","[8]","[9]","[10]","[11]"}));
}

TEST(SymbolBrowserTree, MultiDimensionalIndicesOrderComponentWise) {
    ensureGuiApp();
    std::vector<AdsSymbol> syms{aggregate("MAIN.m", "ARRAY [0..1, 0..10] OF INT")};
    for (const char* i : {"1,10", "0,2", "1,2", "0,10"})
        syms.push_back(leaf(qPrintable(QString("MAIN.m[%1]").arg(i)), "INT", 0));

    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols(syms);
    auto* tv = treeOf(w);
    const auto arr = find(tv, {"MAIN", "m"});
    ASSERT_TRUE(arr.isValid());
    QStringList got;
    for (int r = 0; r < tv->model()->rowCount(arr); ++r)
        got << tv->model()->index(r, 0, arr).data().toString();
    EXPECT_EQ(got, QStringList({"[0,2]", "[0,10]", "[1,2]", "[1,10]"}));
}

// Struct members must NOT be reordered — the type table gives them in
// declaration order, which mirrors the PLC's memory layout.
TEST(SymbolBrowserTree, StructureMembersKeepDeclarationOrder) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.st", "ST_Thing"),
        leaf("MAIN.st.zebra", "LREAL", 0x00),
        leaf("MAIN.st.apple", "LREAL", 0x08),
        leaf("MAIN.st.mango", "LREAL", 0x10),
    });
    auto* tv = treeOf(w);
    const auto st = find(tv, {"MAIN", "st"});
    ASSERT_TRUE(st.isValid());
    QStringList got;
    for (int r = 0; r < tv->model()->rowCount(st); ++r)
        got << tv->model()->index(r, 0, st).data().toString();
    EXPECT_EQ(got, QStringList({"zebra", "apple", "mango"}))
        << "declaration order, not alphabetical";
}

// What the Recorder consults before showing the "add all of these?" prompt.
TEST(SymbolBrowserTree, SelectingAGroupIsReportedSoItCanBeConfirmed) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({
        aggregate("MAIN.axis", "AXIS_REF"),
        leaf("MAIN.axis.PlcToNc.Override", "UDINT", 0x100),
        leaf("MAIN.axis.PlcToNc.ExtSetPos", "LREAL", 0x104),
        leaf("MAIN.speed", "LREAL", 0x300),
    });
    auto* tv = treeOf(w);

    const auto axis = find(tv, {"MAIN", "axis"});
    tv->selectionModel()->select(axis, QItemSelectionModel::ClearAndSelect |
                                       QItemSelectionModel::Rows);
    const auto groups = w.selectedGroupNames();
    ASSERT_EQ(groups.size(), 1);
    EXPECT_EQ(groups.first().toStdString(), "MAIN.axis")
        << "the prompt names the full path the user clicked";

    // A single leaf stands only for itself — no prompt.
    const auto one = find(tv, {"MAIN", "speed"});
    tv->selectionModel()->select(one, QItemSelectionModel::ClearAndSelect |
                                      QItemSelectionModel::Rows);
    EXPECT_TRUE(w.selectedGroupNames().isEmpty());
}

// A listing can come back usable but incomplete — a data-type table the PLC
// only partly served, a structure past the leaf cap. That has to be visible:
// a member missing from the tree with nothing said about it looks exactly
// like a member the PLC never published.
TEST(SymbolBrowserTree, ShowsTheListingNoteOnlyWhenThereIsSomethingToSay) {
    ensureGuiApp();
    scope::recorder::ui::SymbolBrowserWidget w;
    w.setSymbols({leaf("MAIN.speed", "LREAL", 0x300)});
    w.show();

    auto* note = w.findChild<QLabel*>();
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isVisible()) << "nothing to report, nothing shown";

    const QString msg = "MAIN.fbMachine holds more than 32768 recordable values";
    w.setNote(msg);
    EXPECT_TRUE(note->isVisible());
    EXPECT_EQ(note->text().toStdString(), msg.toStdString());
    EXPECT_EQ(note->toolTip().toStdString(), msg.toStdString())
        << "the full text stays reachable when the line is elided";

    w.setNote({});
    EXPECT_FALSE(note->isVisible());
}
