#pragma once

#include "scope/core/IAdsClient.h"
#include "scope/core/SignalStore.h"
#include "scope/core/Hdf5Session.h"
#include "scope/recorder/NotifyChannel.h"
#include "scope/recorder/OversampledChannel.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

namespace scope::recorder {

// Orchestrates a recording: a set of NotifyChannel / OversampledChannel
// instances, the writer timer that drains their queues into Signals and
// into an HDF5 file, and the live-preview signalling.
class RecordingSession : public QObject {
    Q_OBJECT

public:
    explicit RecordingSession(scope::core::SignalStore& store,
                              scope::core::IAdsClient&  client,
                              QObject* parent = nullptr);
    ~RecordingSession();

    // Build channels from a list of selected symbols. Returns false on first
    // setup failure; channels added so far stay armed (call stop() to undo).
    bool addNotifyChannels(const std::vector<NotifyChannel::Config>& configs,
                           QString* errorOut = nullptr);

    bool start(const std::filesystem::path& sessionFile, QString* errorOut = nullptr);
    void stop();
    bool isRunning() const noexcept { return running_.load(); }

    // Aggregate stats for the UI.
    struct Stats {
        std::uint64_t samplesReceived{0};
        std::uint64_t overruns{0};
        std::size_t   channels{0};
    };
    Stats currentStats() const;

signals:
    void statsChanged(scope::recorder::RecordingSession::Stats s);
    void channelOverrun(QString channelName, std::uint64_t totalOverruns);

private:
    void onDrainTick();

    scope::core::SignalStore& store_;
    scope::core::IAdsClient&  client_;
    std::unique_ptr<scope::core::Hdf5Session> hdf5_;

    struct Slot {
        std::unique_ptr<NotifyChannel>     notify;
        std::unique_ptr<OversampledChannel> oversampled;
        std::shared_ptr<scope::core::Signal> signal;
        std::uint64_t lastOverruns{0};
    };
    std::vector<Slot> slots_;

    QTimer drainTimer_;
    std::atomic<bool> running_{false};
};

}  // namespace scope::recorder

Q_DECLARE_METATYPE(scope::recorder::RecordingSession::Stats)
