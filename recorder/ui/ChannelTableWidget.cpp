#include "ChannelTableWidget.h"

#include "scope/style/StyleKit.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace scope::recorder::ui {

namespace {
enum Col {
    ColName = 0,
    ColType,
    ColMode,
    ColCycleUs,
    ColUnit,
    ColIndexGroup,   // hidden, used to build config
    ColIndexOffset,  // hidden
    ColCount
};
}

ChannelTableWidget::ChannelTableWidget(QWidget* parent) : QWidget(parent) {
    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels(
        {"Name", "Type", "Mode", "Cycle [µs]", "Unit", "IG", "IO"});
    table_->setColumnHidden(ColIndexGroup, true);
    table_->setColumnHidden(ColIndexOffset, true);
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    scope::style::installEmptyHint(table_,
        "Symbols you add land here.\n\n"
        "Cycle time and unit are editable per channel before recording.");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 6, 8, 4);
    layout->setSpacing(6);
    layout->addWidget(scope::style::sectionLabel("Recording channels", this));
    layout->addWidget(table_);

    // A user edit to any cell is a document change. Programmatic fills below
    // block the table's signals, so this only fires for real edits.
    connect(table_, &QTableWidget::cellChanged, this,
            [this](int, int){ emit changed(); });
}

void ChannelTableWidget::addChannelFromSymbol(const scope::core::AdsSymbol& sym,
                                              std::uint32_t taskCycleUs) {
    // Fill the cells with signals blocked, then emit one changed() for the
    // whole row rather than one per cell.
    const QSignalBlocker block(table_);
    const int row = table_->rowCount();
    table_->insertRow(row);

    auto setCell = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        if (col == ColIndexGroup || col == ColIndexOffset) {
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        }
        table_->setItem(row, col, item);
    };
    setCell(ColName,        sym.name);
    setCell(ColType,        sym.typeName);
    setCell(ColMode,        "AdsNotify");
    setCell(ColCycleUs,     QString::number(taskCycleUs == 0 ? 1000 : taskCycleUs));
    setCell(ColUnit,        "");
    setCell(ColIndexGroup,  QString::number(sym.indexGroup));
    setCell(ColIndexOffset, QString::number(sym.indexOffset));
    emit changed();
}

void ChannelTableWidget::clear() {
    const QSignalBlocker block(table_);
    table_->setRowCount(0);
}

std::vector<ChannelTableWidget::Row> ChannelTableWidget::rows() const {
    std::vector<Row> out;
    out.reserve(table_->rowCount());
    auto cell = [&](int r, int c) {
        auto* item = table_->item(r, c);
        return item ? item->text() : QString();
    };
    for (int r = 0; r < table_->rowCount(); ++r) {
        Row row;
        row.name        = cell(r, ColName);
        row.type        = cell(r, ColType);
        row.mode        = cell(r, ColMode);
        row.unit        = cell(r, ColUnit);
        row.cycleUs     = cell(r, ColCycleUs).toUInt();
        row.indexGroup  = cell(r, ColIndexGroup).toUInt();
        row.indexOffset = cell(r, ColIndexOffset).toUInt();
        out.push_back(std::move(row));
    }
    return out;
}

void ChannelTableWidget::setRows(const std::vector<Row>& rows) {
    // Programmatic restore (undo / layout load) — keep signals blocked so it
    // doesn't register as a fresh edit.
    const QSignalBlocker block(table_);
    table_->setRowCount(0);
    for (const auto& row : rows) {
        const int r = table_->rowCount();
        table_->insertRow(r);
        auto setCell = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            if (col == ColIndexGroup || col == ColIndexOffset) {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            table_->setItem(r, col, item);
        };
        setCell(ColName,        row.name);
        setCell(ColType,        row.type);
        setCell(ColMode,        row.mode.isEmpty() ? QString("AdsNotify") : row.mode);
        setCell(ColCycleUs,     QString::number(row.cycleUs == 0 ? 1000 : row.cycleUs));
        setCell(ColUnit,        row.unit);
        setCell(ColIndexGroup,  QString::number(row.indexGroup));
        setCell(ColIndexOffset, QString::number(row.indexOffset));
    }
}

std::vector<NotifyChannel::Config> ChannelTableWidget::buildConfigs() const {
    std::vector<NotifyChannel::Config> cfgs;
    cfgs.reserve(table_->rowCount());

    for (int row = 0; row < table_->rowCount(); ++row) {
        NotifyChannel::Config cfg;
        cfg.meta.name           = table_->item(row, ColName)->text();
        cfg.meta.sourceSymbol   = table_->item(row, ColName)->text();
        cfg.meta.unit           = table_->item(row, ColUnit)->text();
        cfg.meta.mode           = scope::core::AcquisitionMode::AdsNotify;
        cfg.meta.parentTaskCycleUs = static_cast<std::uint32_t>(
            table_->item(row, ColCycleUs)->text().toUInt());
        cfg.cycleTimeUs         = cfg.meta.parentTaskCycleUs;
        cfg.indexGroup          = static_cast<std::uint32_t>(
            table_->item(row, ColIndexGroup)->text().toUInt());
        cfg.indexOffset         = static_cast<std::uint32_t>(
            table_->item(row, ColIndexOffset)->text().toUInt());

        // Map the TwinCAT type text to DataType for v1 (REAL/LREAL/DINT/INT/...).
        const QString t = table_->item(row, ColType)->text().trimmed().toUpper();
        if      (t == "LREAL") cfg.meta.dataType = scope::core::DataType::Float64;
        else if (t == "REAL")  cfg.meta.dataType = scope::core::DataType::Float32;
        else if (t == "DINT")  cfg.meta.dataType = scope::core::DataType::Int32;
        else if (t == "UDINT") cfg.meta.dataType = scope::core::DataType::Uint32;
        else if (t == "INT")   cfg.meta.dataType = scope::core::DataType::Int16;
        else if (t == "UINT")  cfg.meta.dataType = scope::core::DataType::Uint16;
        else if (t == "BOOL")  cfg.meta.dataType = scope::core::DataType::Bool;
        else                   cfg.meta.dataType = scope::core::DataType::Float64;

        if (cfg.meta.parentTaskCycleUs > 0) {
            cfg.meta.sampleRateHz = 1'000'000.0 / cfg.meta.parentTaskCycleUs;
        }
        cfgs.push_back(std::move(cfg));
    }
    return cfgs;
}

}  // namespace scope::recorder::ui
