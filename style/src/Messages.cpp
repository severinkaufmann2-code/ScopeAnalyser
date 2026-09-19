#include "scope/style/Messages.h"

#include <QWidget>

namespace scope::style {
namespace {

// Enough to recognise the pattern ("ah — it's the whole aStation array"),
// short enough that the dialog stays a dialog. The rest is one click away.
constexpr int kInlineItems = 8;

}  // namespace

int listMessageInlineItems() { return kInlineItems; }

void fillListMessage(QMessageBox&       box,
                     const QString&     lead,
                     const QStringList& items,
                     const QString&     advice) {
    QString text = lead;
    if (!items.isEmpty()) {
        text += "\n\n" + items.mid(0, kInlineItems).join("\n");
        if (items.size() > kInlineItems) {
            text += QString("\n… and %1 more.").arg(items.size() - kInlineItems);
            // The full list, in the box Qt gives a fixed height and a
            // scrollbar — and which the user can select and copy out of.
            box.setDetailedText(items.join("\n"));
        }
    }
    box.setText(text);
    if (!advice.isEmpty()) box.setInformativeText(advice);
}

QMessageBox::StandardButton listMessage(QWidget*                     parent,
                                        QMessageBox::Icon            icon,
                                        const QString&               title,
                                        const QString&               lead,
                                        const QStringList&           items,
                                        const QString&               advice,
                                        QMessageBox::StandardButtons buttons,
                                        QMessageBox::StandardButton  defaultButton) {
    QMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    fillListMessage(box, lead, items, advice);
    box.setStandardButtons(buttons);
    if (defaultButton != QMessageBox::NoButton) box.setDefaultButton(defaultButton);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

}  // namespace scope::style
