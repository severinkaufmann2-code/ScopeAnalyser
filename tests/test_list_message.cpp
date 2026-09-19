// Messages that carry a list — "these 412 symbols can't be recorded", "these
// 38 channels repeat timestamps".
//
// A QMessageBox grows to fit its text and stops at nothing: one line per
// channel and the dialog is taller than the screen, with its own buttons off
// the bottom, which is exactly the situation the message is reporting. The
// list belongs in the detail box, which Qt gives a fixed height and a
// scrollbar.

#include "scope/style/Messages.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMessageBox>
#include <QStringList>

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

QStringList manySymbols(int n) {
    QStringList out;
    for (int i = 0; i < n; ++i)
        out << QString("MAIN.fbMachine.aStation[%1].sName  (STRING(80))").arg(i);
    return out;
}

}  // namespace

TEST(ListMessage, ShowsTheCountAndASampleAndKeepsTheRestInTheDetailBox) {
    ensureGuiApp();
    const auto items = manySymbols(412);

    QMessageBox box;
    scope::style::fillListMessage(
        box, "412 of the selected symbols have no single numeric value:", items,
        "Record the members instead.");

    const int shown = scope::style::listMessageInlineItems();
    EXPECT_EQ(box.text().count('\n'), shown + 2)
        << "lead, a blank line, the sample, and the '… and N more' tail";
    EXPECT_TRUE(box.text().contains(items.at(shown - 1)));
    EXPECT_FALSE(box.text().contains(items.at(shown)))
        << "past the sample the message says how many, not which";
    EXPECT_TRUE(box.text().contains(QString("and %1 more").arg(412 - shown)));

    // Everything is still reachable — and copyable — one click away.
    EXPECT_TRUE(box.detailedText().contains(items.first()));
    EXPECT_TRUE(box.detailedText().contains(items.last()));
    EXPECT_EQ(box.informativeText().toStdString(), "Record the members instead.");
}

TEST(ListMessage, AShortListIsJustTheMessage) {
    ensureGuiApp();
    const auto items = manySymbols(3);

    QMessageBox box;
    scope::style::fillListMessage(box, "3 symbols were not added:", items);

    for (const auto& i : items) EXPECT_TRUE(box.text().contains(i));
    EXPECT_TRUE(box.detailedText().isEmpty())
        << "nothing hidden, so no “Show Details” to hunt through";
}

// The regression this exists for: the dialog has to stay a dialog.
TEST(ListMessage, StaysOnTheScreenWhereTheRawListWouldNot) {
    ensureGuiApp();
    const auto items = manySymbols(400);

    QMessageBox capped;
    scope::style::fillListMessage(capped, "400 symbols were not added:", items);

    QMessageBox raw;   // what a plain join() into the message text gives
    raw.setText("400 symbols were not added:\n\n" + items.join("\n"));

    const int cappedH = capped.sizeHint().height();
    const int rawH    = raw.sizeHint().height();
    EXPECT_LT(cappedH, 500) << "fits on any screen anyone runs this on";
    EXPECT_GT(rawH, 4 * cappedH)
        << "the unbounded version is the bug — if it ever stops being taller, "
           "this test is measuring the wrong thing";
}
