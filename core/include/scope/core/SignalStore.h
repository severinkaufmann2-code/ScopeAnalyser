#pragma once

#include "Signal.h"

#include <QHash>
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

    // Re-key a channel. Returns false — changing nothing — when `from` is
    // absent, when `to` is already taken (never clobbers) or when `to` is
    // blank. The signal is CLONED under the new name rather than renamed in
    // place, because undo snapshots elsewhere hold shared_ptrs to it and
    // re-add them keyed by their own meta; mutating the meta would silently
    // rewrite history.
    //
    // Emits channelRenamed, deliberately NOT channelRemoved + channelAdded:
    // listeners treat a removal as the channel going away and tear down
    // formulas, plot rows and XY selections that a rename must preserve.
    bool rename(const QString& from, const QString& to);

    std::shared_ptr<Signal> get(const QString& name) const;
    bool contains(const QString& name) const;
    QStringList channelNames() const;
    std::size_t size() const;

    // A Signal grows in place via Signal::append() (e.g. the recorder's
    // drain loop), which the store can't observe on its own. The owner of
    // the growing signal calls this so views (Analyser) can refresh.
    // Safe to call when `name` isn't in the store — it just emits.
    void notifyDataChanged(const QString& name) { emit channelDataChanged(name); }

signals:
    void channelAdded(QString name);
    void channelRemoved(QString name);
    void channelRenamed(QString from, QString to);
    // Emitted when an existing channel's sample data changed (appended /
    // recomputed) without the channel itself being added or removed.
    void channelDataChanged(QString name);

private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<QString, std::shared_ptr<Signal>> map_;
};

}  // namespace scope::core
