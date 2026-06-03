#include "ChannelTableWidget.h"

#include <QHBoxLayout>
#include <QHeaderView>
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

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(table_);
}

void ChannelTableWidget::addChannelFromSymbol(const scope::core::AdsSymbol& sym,
                                              std::uint32_t taskCycleUs) {
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
}

void ChannelTableWidget::clear() { table_->setRowCount(0); }

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
