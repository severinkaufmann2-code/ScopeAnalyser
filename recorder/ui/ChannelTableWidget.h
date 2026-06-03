#pragma once

#include "scope/recorder/NotifyChannel.h"

#include <QWidget>
#include <QTableWidget>

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

private:
    QTableWidget* table_;
};

}  // namespace scope::recorder::ui
