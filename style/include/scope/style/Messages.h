#pragma once

#include <QMessageBox>
#include <QString>
#include <QStringList>

class QWidget;

namespace scope::style {

// A message whose body is a list: "these 412 symbols can't be recorded",
// "these 38 channels repeat timestamps", "these 12 files produced no signals".
//
// A QMessageBox grows to fit its text and stops at nothing — a list with one
// line per channel makes a dialog taller than the screen, and its buttons go
// off the bottom with it, so the user can neither read the message nor
// dismiss it. The count and the first few items go in the message; the whole
// list goes to the dialog's detail box, which scrolls at a fixed height and
// can be copied out.
//
// `lead` says what happened and how many, `items` is one line each, `advice`
// is what to do about it. Returns the button pressed, so this also asks.
//
// fillListMessage() is the same shaping without the showing, for a dialog a
// caller wants to set up further (custom buttons, a checkbox) first.
void fillListMessage(QMessageBox&       box,
                     const QString&     lead,
                     const QStringList& items,
                     const QString&     advice = {});

QMessageBox::StandardButton listMessage(
    QWidget*                     parent,
    QMessageBox::Icon            icon,
    const QString&               title,
    const QString&               lead,
    const QStringList&           items,
    const QString&               advice        = {},
    QMessageBox::StandardButtons buttons       = QMessageBox::Ok,
    QMessageBox::StandardButton  defaultButton = QMessageBox::NoButton);

// How many items listMessage shows without the user opening the details.
int listMessageInlineItems();

}  // namespace scope::style
