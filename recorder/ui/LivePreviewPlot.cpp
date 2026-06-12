#include "LivePreviewPlot.h"

#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"
#include "scope/style/StyleKit.h"

#include <qcustomplot.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <filesystem>
#include <limits>

namespace scope::recorder::ui {

namespace {
enum Col { ColVis = 0, ColName, ColAxis, ColCount };
constexpr int kTickIntervalMs = 50;
}

LivePreviewPlot::LivePreviewPlot(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    scope_ = new scope::plot::ScopePlot(this);
    scope_->plot()->xAxis->setLabel("seconds before now");
    scope_->setPauseSupported(true);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"", "Channel", "Axis"});
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColVis,  QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColAxis, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    style::installEmptyHint(table_,
        "Channels appear here as soon as they land in the signal store "
        "(recorded, imported, or derived).");

    auto smallBtn = [this](style::Glyph glyph, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(style::icon(glyph));
        b->setToolTip(tip);
        return b;
    };
    auto* addAxisBtn = smallBtn(style::Glyph::Plus,
        "Add a Y axis (alternates left / right).");
    auto* delAxisBtn = smallBtn(style::Glyph::Minus,
        "Remove the last Y axis (it must have no channels assigned).");
    auto* saveBtn    = new QPushButton("Save layout…", this);
    auto* loadBtn    = new QPushButton("Load layout…", this);

    auto* axHeader = new QHBoxLayout();
    axHeader->setSpacing(2);
    axHeader->addWidget(scope::style::sectionLabel("Y axes", this));
    axHeader->addStretch();
    axHeader->addWidget(addAxisBtn);
    axHeader->addWidget(delAxisBtn);

    auto* layoutBtnRow = new QHBoxLayout();
    layoutBtnRow->addWidget(saveBtn);
    layoutBtnRow->addWidget(loadBtn);

    auto* sidePanel = new QWidget(this);
    sidePanel->setMinimumWidth(220);
    auto* sidebar = new QVBoxLayout(sidePanel);
    sidebar->setContentsMargins(8, 6, 4, 8);
    sidebar->setSpacing(6);
    sidebar->addWidget(scope::style::sectionLabel("Live channels", this));
    sidebar->addWidget(table_, /*stretch=*/1);
    sidebar->addLayout(axHeader);
    sidebar->addLayout(layoutBtnRow);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(sidePanel);
    split->addWidget(scope_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({280, 1100});

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->addWidget(split);

    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &LivePreviewPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &LivePreviewPlot::onChannelRemoved);
    connect(scope_, &scope::plot::ScopePlot::yAxesChanged,
            this, &LivePreviewPlot::rebuildAxisCombos);
    connect(scope_, &scope::plot::ScopePlot::themePaletteChanged,
            this, [this]{ recolorChannels(); });

    connect(addAxisBtn, &QToolButton::clicked, this, [this]{ scope_->addYAxis(); });
    connect(delAxisBtn, &QToolButton::clicked, this, [this]{
        const int last = scope_->yAxisCount() - 1;
        if (last <= 0) return;
        QString err;
        if (!scope_->removeYAxis(last, &err)) {
            QMessageBox::information(this, "Can't remove", err);
            return;
        }
        recolorChannels();
    });
    connect(saveBtn, &QPushButton::clicked, this, &LivePreviewPlot::saveLayoutDialog);
    connect(loadBtn, &QPushButton::clicked, this, &LivePreviewPlot::loadLayoutDialog);

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &LivePreviewPlot::onTick);
    tick->start(kTickIntervalMs);
}

void LivePreviewPlot::setWindowSeconds(double seconds) { windowSeconds_ = seconds; }

int LivePreviewPlot::pickAxisForUnit(const QString& unit) const {
    if (unit.isEmpty()) return 0;
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        if (ax && ax->label().contains(unit, Qt::CaseInsensitive)) return i;
    }
    return 0;
}

int LivePreviewPlot::axisIndexForRow(int row) const {
    if (row < 0 || row >= table_->rowCount()) return 0;
    auto* co = qobject_cast<QComboBox*>(table_->cellWidget(row, ColAxis));
    return co ? co->currentIndex() : 0;
}

void LivePreviewPlot::setAxisIndexForRow(int row, int axisIndex) {
    auto* co = qobject_cast<QComboBox*>(table_->cellWidget(row, ColAxis));
    if (!co) return;
    if (axisIndex < 0) axisIndex = 0;
    if (axisIndex >= co->count()) axisIndex = co->count() - 1;
    co->setCurrentIndex(axisIndex);
}

QSet<QString> LivePreviewPlot::activeChannels() const {
    QSet<QString> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) {
            if (cb->isChecked()) out.insert(table_->item(r, ColName)->text());
        }
    }
    return out;
}

void LivePreviewPlot::recolorChannels() {
    QHash<int, int> perAxis;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* nameItem = table_->item(r, ColName);
        const QString name = nameItem->text();
        if (!graphs_.contains(name)) continue;
        const int axisIdx = axisIndexForRow(r);
        const int onAxis  = perAxis[axisIdx]++;
        const QColor c = scope_->deriveChannelColor(axisIdx, onAxis);
        graphs_[name]->setPen(QPen(c));
        nameItem->setIcon(scope::style::colorSwatch(c));
    }
}

void LivePreviewPlot::rebuildTable() {
    QHash<QString, std::pair<bool,int>> prev;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName) ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = true; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis))) axis = co->currentIndex();
        prev[name] = {vis, axis};
    }

    table_->setRowCount(0);
    for (const auto& name : store_.channelNames()) {
        const int r = table_->rowCount();
        table_->insertRow(r);

        auto* vis = new QCheckBox();
        bool defaultVis = true; int defaultAxis = 0;
        if (prev.contains(name)) {
            defaultVis  = prev[name].first;
            defaultAxis = prev[name].second;
        } else if (pendingAssignments_.contains(name)) {
            defaultAxis = pendingAssignments_.take(name);
        } else {
            auto s = store_.get(name);
            if (s) defaultAxis = pickAxisForUnit(s->meta().unit);
        }
        vis->setChecked(defaultVis);
        table_->setCellWidget(r, ColVis, vis);
        connect(vis, &QCheckBox::toggled, this, [this](bool){ onVisibilityChanged(); });

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, ColName, nameItem);

        auto* combo = new QComboBox();
        for (int a = 0; a < scope_->yAxisCount(); ++a)
            combo->addItem(scope_->yAxis(a)->label());
        combo->setCurrentIndex(std::min(defaultAxis, combo->count() - 1));
        table_->setCellWidget(r, ColAxis, combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, r](int idx){ onAxisComboChanged(r, idx); });
    }
}

void LivePreviewPlot::rebuildAxisCombos() {
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis));
        if (!combo) continue;
        const int cur = combo->currentIndex();
        QSignalBlocker blocker(combo);
        combo->clear();
        for (int a = 0; a < scope_->yAxisCount(); ++a)
            combo->addItem(scope_->yAxis(a)->label());
        combo->setCurrentIndex(std::min(std::max(cur, 0), combo->count() - 1));
    }
}

void LivePreviewPlot::onAxisComboChanged(int row, int axisIndex) {
    if (row < 0 || row >= table_->rowCount()) return;
    const QString name = table_->item(row, ColName)->text();
    if (!graphs_.contains(name)) return;
    scope_->setGraphYAxis(graphs_[name], axisIndex);
    recolorChannels();
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void LivePreviewPlot::onVisibilityChanged() {
    const auto active = activeChannels();
    for (auto it = graphs_.begin(); it != graphs_.end(); ++it) {
        it.value()->setVisible(active.contains(it.key()));
    }
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void LivePreviewPlot::onChannelAdded(QString name) {
    if (graphs_.contains(name)) return;
    auto* plot = scope_->plot();
    auto sig = store_.get(name);

    int axisIdx = 0;
    if (pendingAssignments_.contains(name)) {
        axisIdx = pendingAssignments_.take(name);
    } else if (sig) {
        axisIdx = pickAxisForUnit(sig->meta().unit);
    }
    if (axisIdx >= scope_->yAxisCount()) axisIdx = 0;

    auto* g = plot->addGraph(plot->xAxis, scope_->yAxis(axisIdx));
    g->setName(name);
    // Adaptive sampling stays at its QCustomPlot default (true) for
    // smooth pan/zoom on long live-preview windows.
    scope_->applyLineDisplayModeTo(g);
    graphs_[name] = g;
    rebuildTable();
    recolorChannels();
}

void LivePreviewPlot::onChannelRemoved(QString name) {
    auto it = graphs_.find(name);
    if (it != graphs_.end()) {
        scope_->plot()->removeGraph(it.value());
        graphs_.erase(it);
    }
    rebuildTable();
    recolorChannels();
}

void LivePreviewPlot::onTick() {
    if (scope_->isPaused()) return;
    if (graphs_.empty()) return;

    const auto nowNs    = scope::core::nowNs();
    const auto windowNs = static_cast<scope::core::TimestampNs>(windowSeconds_ * 1e9);
    const auto cutoffNs = nowNs - windowNs;

    const auto active = activeChannels();

    for (auto it = graphs_.begin(); it != graphs_.end(); ++it) {
        const QString& name = it.key();
        QCPGraph*      graph = it.value();
        if (!active.contains(name)) continue;
        auto sig = store_.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        auto values = sig->readAsDouble();

        QVector<double> xs, ys;
        xs.reserve(static_cast<int>(view.count));
        ys.reserve(static_cast<int>(view.count));
        for (std::size_t i = 0; i < view.count; ++i) {
            if (view.timestamps[i] < cutoffNs) continue;
            xs.push_back((view.timestamps[i] - nowNs) / 1e9);
            const double v = (i < values.size()) ? values[i] : 0.0;
            ys.push_back(v);
        }
        graph->setData(xs, ys, /*alreadySorted=*/true);
    }

    auto* plot = scope_->plot();
    plot->xAxis->setRange(-windowSeconds_, 0.0);
    scope_->rescaleAllYAxes();
    plot->replot(QCustomPlot::rpQueuedReplot);
}

void LivePreviewPlot::saveLayoutDialog() {
    QFileDialog dlg(this, "Save plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"Scope plot layout (*.scolayout)", "All files (*)"});
    dlg.setDefaultSuffix("scolayout");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();
    if (!path.endsWith(".scolayout", Qt::CaseInsensitive)) path += ".scolayout";

    scope::plot::PlotLayout layout;
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        scope::plot::PlotLayoutAxis a;
        a.label = ax->label();
        a.side  = (ax->axisType() == QCPAxis::atRight) ? "right" : "left";
        a.hasRange = true;
        a.min = ax->range().lower;
        a.max = ax->range().upper;
        layout.axes.append(a);
    }
    for (int r = 0; r < table_->rowCount(); ++r) {
        scope::plot::PlotLayoutChannel c;
        c.name = table_->item(r, ColName)->text();
        c.axisIndex = axisIndexForRow(r);
        layout.channels.append(c);
    }
    QString err;
    if (!layout.saveToFile(std::filesystem::path(path.toStdString()), &err))
        QMessageBox::critical(this, "Save failed", err);
}

void LivePreviewPlot::loadLayoutDialog() {
    QFileDialog dlg(this, "Load plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setNameFilters({"Scope plot layout (*.scolayout)", "All files (*)"});
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;

    QString err;
    auto layout = scope::plot::PlotLayout::loadFromFile(
        std::filesystem::path(sel.first().toStdString()), &err);
    if (!err.isEmpty()) {
        QMessageBox::critical(this, "Load failed", err);
        return;
    }

    while (scope_->yAxisCount() > 1) {
        const int idx = scope_->yAxisCount() - 1;
        for (int g = 0; g < scope_->plot()->graphCount(); ++g) {
            if (scope_->plot()->graph(g)->valueAxis() == scope_->yAxis(idx))
                scope_->setGraphYAxis(scope_->plot()->graph(g), 0);
        }
        QString rmErr;
        if (!scope_->removeYAxis(idx, &rmErr)) break;
    }
    for (int i = 0; i < layout.axes.size(); ++i) {
        const auto& la = layout.axes[i];
        const Qt::Alignment side = (la.side == "right") ? Qt::AlignRight : Qt::AlignLeft;
        if (i == 0) {
            scope_->yAxis(0)->setLabel(la.label);
            if (la.hasRange) scope_->yAxis(0)->setRange(la.min, la.max);
        } else {
            const int newIdx = scope_->addYAxis(la.label, side);
            if (la.hasRange) scope_->yAxis(newIdx)->setRange(la.min, la.max);
        }
    }
    rebuildAxisCombos();

    pendingAssignments_.clear();
    for (const auto& c : layout.channels) {
        bool found = false;
        for (int r = 0; r < table_->rowCount(); ++r) {
            if (table_->item(r, ColName)->text() == c.name) {
                setAxisIndexForRow(r, c.axisIndex);
                found = true;
                break;
            }
        }
        if (!found) pendingAssignments_.insert(c.name, c.axisIndex);
    }
    recolorChannels();
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::recorder::ui
