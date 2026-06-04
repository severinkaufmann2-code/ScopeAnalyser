#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QDialog>

class QLineEdit;
class QComboBox;

namespace scope::converter::ui {

// Modal dialog to add or edit a single channel. The user types the column
// letter (A, B, ...), an optional row range ("5:100", "5:", ":100", "all"
// or empty for all data rows), picks a role, and supplies a name + unit.
class ChannelEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelEditDialog(QWidget* parent = nullptr);

    // Populate the dialog from an existing ColumnMapping (for "Edit" mode).
    void setMapping(const ColumnMapping& m);

    // Read back the user's input. Returns false if the input couldn't be
    // parsed (and sets *errorOut to a human-readable message).
    bool getMapping(ColumnMapping* out, QString* errorOut = nullptr) const;

private:
    QLineEdit* colEdit_;
    QLineEdit* rowsEdit_;
    QComboBox* roleCombo_;
    QLineEdit* nameEdit_;
    QLineEdit* unitEdit_;
};

}  // namespace scope::converter::ui
