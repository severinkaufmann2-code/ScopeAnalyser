#pragma once

#include "Signal.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace scope::core {

// Process-wide store of named Signal pointers. The three tools (Recorder,
// Analyser, Converter) all bind to one instance: recordings appear in the
// Analyser the instant they're added, converters can push imported data in,
// and derived channels written by the Analyser show up in plots.
//
// Thread-safety: shared_mutex guards the map. Signal objects themselves are
// independently thread-safe.
class SignalStore : public QObject {
    Q_OBJECT

public:
    explicit SignalStore(QObject* parent = nullptr);

    // Insert or replace. Replacing emits `channelRemoved` then `channelAdded`.
    void add(std::shared_ptr<Signal> signal);
    void remove(const QString& name);
    void clear();

    std::shared_ptr<Signal> get(const QString& name) const;
    bool contains(const QString& name) const;
    QStringList channelNames() const;
    std::size_t size() const;

signals:
    void channelAdded(QString name);
    void channelRemoved(QString name);

private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<QString, std::shared_ptr<Signal>> map_;
};

}  // namespace scope::core
