#pragma once

#include "scope/recorder/NotifyChannel.h"

#include <QString>
#include <QWidget>
#include <QTableWidget>

#include <cstdint>
#include <vector>

namespace scope::recorder::ui {

// Per-channel table: name, type, mode, sample rate (Hz), unit, scaling, color.
// The first row that lands here is auto-filled from the selected AdsSymbol;
// the user can edit mode, rate, unit, and color before starting the
// recording.
class ChannelTableWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChannelTableWidget(QWidget* parent = nullptr);

    void addChannelFromSymbol(const scope::core::AdsSymbol& sym, std::uint32_t taskCycleUs);
    void clear();
    std::vector<NotifyChannel::Config> buildConfigs() const;

    // Lossless view of the table for save/restore (recorder layout). Carries
    // the raw cell text incl. the hidden IndexGroup / IndexOffset, so a
    // restored row records exactly like the original once reconnected.
    struct Row {
        QString       name;
        QString       type;
        QString       mode;
        QString       unit;
        std::uint32_t cycleUs{0};
        std::uint32_t indexGroup{0};
        std::uint32_t indexOffset{0};
    };
    std::vector<Row> rows() const;
    void setRows(const std::vector<Row>& rows);

signals:
    // Emitted on a user edit (a symbol added, or a cell edited). NOT emitted
    // for programmatic fills (setRows / clear) so an undo-restore or layout
    // load doesn't record a fresh history entry.
    void changed();

private:
    QTableWidget* table_;
};

}  // namespace scope::recorder::ui
