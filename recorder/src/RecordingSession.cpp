#include "scope/recorder/RecordingSession.h"

#include <spdlog/spdlog.h>

namespace scope::recorder {

using scope::core::Signal;

namespace {
constexpr int kDrainIntervalMs = 50;  // 20 Hz writer/UI tick
}

RecordingSession::RecordingSession(scope::core::SignalStore& store,
                                   scope::core::IAdsClient&  client,
                                   QObject* parent)
    : QObject(parent), store_(store), client_(client) {
    qRegisterMetaType<Stats>("scope::recorder::RecordingSession::Stats");
    drainTimer_.setInterval(kDrainIntervalMs);
    connect(&drainTimer_, &QTimer::timeout, this, &RecordingSession::onDrainTick);
}

RecordingSession::~RecordingSession() { stop(); }

bool RecordingSession::addNotifyChannels(const std::vector<NotifyChannel::Config>& configs,
                                         QString* errorOut) {
    for (const auto& cfg : configs) {
        Slot slot;
        slot.signal = std::make_shared<Signal>(cfg.meta);
        slot.notify = std::make_unique<NotifyChannel>(cfg);
        store_.add(slot.signal);
        slots_.push_back(std::move(slot));
    }
    Q_UNUSED(errorOut);
    return true;
}

bool RecordingSession::start(const std::filesystem::path& sessionFile, QString* errorOut) {
    if (running_.load()) return true;

    hdf5_ = scope::core::Hdf5Session::create(sessionFile, errorOut);
    if (!hdf5_) return false;

    for (auto& s : slots_) {
        if (!hdf5_->addChannel(s.signal->meta(), errorOut)) {
            spdlog::error("HDF5 addChannel failed: {}",
                          errorOut ? errorOut->toStdString() : "");
        }
        if (s.notify) {
            QString err;
            if (!s.notify->arm(client_, &err)) {
                if (errorOut) *errorOut = "arm: " + err;
                stop();
                return false;
            }
        }
    }

    running_ = true;
    drainTimer_.start();
    return true;
}

void RecordingSession::stop() {
    if (!running_.load() && slots_.empty()) return;
    drainTimer_.stop();
    running_ = false;
    for (auto& s : slots_) {
        if (s.notify) s.notify->disarm(client_);
    }
    if (hdf5_) {
        hdf5_->flush();
    }
    // One final drain so we don't lose what was in flight.
    onDrainTick();
    hdf5_.reset();
}

void RecordingSession::onDrainTick() {
    Stats stats;
    for (auto& s : slots_) {
        if (!s.notify || !s.signal) continue;
        const auto preCount = s.signal->sampleCount();
        const auto drained = s.notify->drainTo(*s.signal);
        if (drained > 0 && hdf5_) {
            auto view = s.signal->snapshotForRead();
            QString err;
            if (!hdf5_->appendSamples(
                    s.signal->meta().name,
                    view.timestamps + preCount,
                    view.values    + preCount * s.signal->bytesPerSample(),
                    drained, &err)) {
                spdlog::error("HDF5 append failed: {}", err.toStdString());
            }
        }
        stats.samplesReceived += s.notify->samplesReceived();
        const auto overruns = s.notify->overruns();
        if (overruns > s.lastOverruns) {
            emit channelOverrun(s.signal->meta().name, overruns);
            s.lastOverruns = overruns;
        }
        stats.overruns += overruns;
    }
    stats.channels = slots_.size();
    emit statsChanged(stats);
}

RecordingSession::Stats RecordingSession::currentStats() const {
    Stats s;
    for (const auto& slot : slots_) {
        if (!slot.notify) continue;
        s.samplesReceived += slot.notify->samplesReceived();
        s.overruns        += slot.notify->overruns();
    }
    s.channels = slots_.size();
    return s;
}

}  // namespace scope::recorder
