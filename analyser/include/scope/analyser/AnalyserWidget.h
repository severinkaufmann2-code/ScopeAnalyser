#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

#include <memory>

class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QTextBrowser;
class QLabel;

namespace scope::analyser {

class FormulaEngine;

namespace ui { class AnalyserPlot; }

// Formula editor + autocomplete + function reference + plot. Bound to the
// shared SignalStore so recordings flow in automatically.
class AnalyserWidget : public QWidget {
    Q_OBJECT
public:
    explicit AnalyserWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);
    ~AnalyserWidget();

private slots:
    void onEvaluateClicked();
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);
    void onChannelDoubleClicked();

private:
    void refreshChannelList();
    void buildHelpPanel();

    scope::core::SignalStore& store_;
    std::unique_ptr<FormulaEngine> engine_;

    QPlainTextEdit* editor_;
    QPushButton*    evalBtn_;
    QListWidget*    channelList_;
    QTextBrowser*   helpBrowser_;
    QLabel*         statusLabel_;
    ui::AnalyserPlot* plot_;
};

}  // namespace scope::analyser
